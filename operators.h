// THE DECLARED OPERATOR TABLE: reading the cards instead of observing them.
//
// THE FACT THAT JUSTIFIES THIS MODULE. Of the 24 cards in benchmark A's deck,
// exactly TWO grant a state that unlocks anything at all: `EFFECT_ADD_CODE`
// (Kaleido Chick) and `EFFECT_EXTRA_FUSION_MATERIAL` (Masquerade). Those are
// EXACTLY the two measured bottlenecks (0.14 % and 0 %). They are found by a
// `grep` over two of the game's constants, without naming a single card.
//
// And a census of the `CATEGORY_*` values WOULD NOT HAVE FOUND THEM: it returns
// `CATEGORY_FUSION_SUMMON` once (Wolf) and not a word about the two pivots. TWO
// VOCABULARIES are needed:
//
//   CATEGORY_*  describes what the effect does to CARDS  (send, search)
//   EFFECT_*    describes which STATE it grants          (rename, allow a
//                                                        material from the grave)
//
// The combo rests entirely on the second one. THAT IS THE RECIPE GRAPH'S
// MISTAKE: it models PRODUCTS, never GRANTED STATES. `--backward` was the right
// algorithm on a graph with its edges amputated; the missing edges are the
// effects, and they are DECLARED.
//
// WHAT THIS MODULE IS, AND WHAT IT IS NOT. It is a STATIC ANALYSIS of the Lua:
// it reads the DECLARATION, not the semantics. Conditions and costs are
// closures; their `chk == 0` is not evaluated here. So the table is OPTIMISTIC:
// it says what a card declares it can do, never what it can do AT THIS INSTANT.
// That is enough for a validation harness and to feed a graph; it is not enough
// to decide legality, and the core remains the sole judge of that.
//
// NO CARD IS NAMED HERE. The constants come from the game's own `constant.lua`,
// served by the SAME ScriptProvider as the core: a script-set mismatch
// therefore shifts the table TOO, instead of making it lie silently.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets.h"

namespace solver {

// --- THE GAME'S CONSTANTS, READ FROM THE GAME --------------------------------
//
// `constant.lua` and `archetype_setcode_constants.lua` are game files, resolved
// by --scriptdir like any card script. Reading them there rather than copying
// them here has a consequence that is not cosmetic: a constant whose value
// changes between two script versions changes value in the table, and a MISSING
// constant makes evaluation fail instead of returning zero.
class ConstantTable {
public:
	// Returns the number of constants loaded. Missing files are not an error (a
	// minimal scriptdir may not carry them): it is `Size() == 0` that must alert
	// the caller.
	size_t Load(ScriptProvider& sp);

	bool Lookup(const std::string& name, uint64_t& out) const;

	// Evaluates a constant expression as it appears in the scripts: `A`, `A+B`,
	// `A|B`, `0x40`, `113`. Returns FALSE when a token is unknown; we NEVER guess
	// the value of a missing constant, on pain of manufacturing an operator that
	// does not exist.
	bool Eval(const std::string& expr, uint64_t& out) const;

	// Name of the constant in this family that carries exactly this value (empty
	// string when there is none). For PRINTING only: a composite mask is
	// decomposed by the function below.
	std::string NameOf(const std::string& prefix, uint64_t value) const;
	// Decomposes a mask into the bit names of the family: "LOCATION_MZONE|
	// LOCATION_SZONE" rather than "0xc".
	std::string MaskNames(const std::string& prefix, uint64_t value) const;

	// VALUE OF A CARD STRING, as `Auxiliary.Stringid` builds it.
	//
	// THE SHIFT IS READ FROM `utility.lua`, NOT ASSUMED. The convention is not
	// "aux.Stringid(id, n) = id * 16 + n" (which is what the recovery of the code
	// of a `MSG_SELECT_YESNO` used to rest on); contemporary scripts do
	// `(n & 0xfffff) | code << 20`. A harness that assumes the old convention
	// returns "0 matched out of 37" and reads like an EXTRACTION failure when it is
	// a failure of the HYPOTHESIS.
	uint64_t StringId(uint32_t code, uint32_t index) const;
	uint32_t StringShift() const { return string_shift; }
	bool StringShiftRead() const { return string_shift_read; }

	size_t Size() const { return vals.size(); }
	const std::vector<std::string>& Files() const { return files; }

private:
	void Absorb(const std::vector<char>& src);
	void ReadStringId(const std::vector<char>& src);
	std::unordered_map<std::string, uint64_t> vals;
	std::vector<std::pair<std::string, uint64_t>> order;
	std::vector<std::string> files;
	uint32_t string_shift = 20;
	uint64_t string_mask = 0xfffff;
	bool string_shift_read = false;
};

// --- ONE DECLARED EFFECT -----------------------------------------------------
//
// The shape is regular across all the scripts: an `Effect.CreateEffect`, a run
// of `SetXxx`, a `RegisterEffect`. It is that regularity, and not a per-card
// table, that makes the extraction generic.
struct DeclaredEffect {
	std::string var;          // e1, e2... (local identifier, for the report)
	std::string in_function;  // the function that creates it ("initial_effect", ...)
	int line = 0;
	bool registered = false;  // c:RegisterEffect / Duel.RegisterEffect
	// Declared in `initial_effect`: this is an OPERATOR, something the player can
	// employ. Declared elsewhere (in an operation, in a cost): this is a GRANTED
	// STATE, the result of an operator.
	bool at_init = false;

	uint64_t etype = 0;       // EFFECT_TYPE_* (mask)
	uint64_t range = 0;       // LOCATION_* where the effect is USABLE (SetRange)
	uint64_t property = 0;    // EFFECT_FLAG_*
	uint64_t target_range_self = 0, target_range_opp = 0;

	// `SetCode` carries TWO things depending on the effect's type, and that is
	// exactly the distinction between the two vocabularies: a trigger puts an
	// `EVENT_*` there (when), a continuous effect puts an `EFFECT_*` there (which
	// state it grants). We separate them not by semantics but by the token's
	// PREFIX, which is exact and assumes nothing.
	std::string code_name;
	uint64_t code_value = 0;
	bool code_is_event = false;
	bool code_is_effect = false;

	uint64_t category = 0;    // CATEGORY_* (mask)

	// Description: `aux.Stringid(id, n)`, the universal convention of the scripts.
	// THIS IS THE PROMPT'S IDENTITY: the same (code, desc) pair the core's messages
	// carry, hence the harness's matching key.
	bool has_desc = false;
	uint32_t desc_card = 0;   // `id` (0 when the description is not a Stringid)
	uint32_t desc_index = 0;  // `n`
	uint64_t desc_value = 0;  // the full value, as the message carries it
	bool desc_is_system = false;   // numeric description (system string)

	// Declared resource. `SetCountLimit(1)` = once per turn and PER COPY;
	// `SetCountLimit(1, id)` = once per turn and per NAME. The presence of the
	// second argument makes all the difference, and it is declared.
	bool has_count_limit = false;
	uint32_t count_limit = 0;
	bool count_by_name = false;
	std::string count_tag;

	// Wired functions. Their mere PRESENCE is already a precondition: an effect
	// with `SetCost` has a cost to pay, an effect with `SetCondition` has a guard.
	// What those closures test is not statically readable.
	std::string fn_cost, fn_condition, fn_target, fn_operation, fn_value;
	// Non-empty when the operator is NOT declared in the card's script but by a
	// game PROCEDURE (`Pendulum.AddProcedure`, `Fusion.AddProcSpell`...). Without
	// reading those, setting a Pendulum scale and activating a Polymerization have
	// NO declared operator, and the harness counts them as failures for a reason
	// that has nothing to do with extraction.
	std::string from_proc;
};

// What an operator PRODUCES, as the script declares it to the core through
// `Duel.SetOperationInfo(chain, CATEGORY_x, targets, count, player, LOCATION_y)`.
// It is the only PRODUCT declaration the scripts carry, and it gives the
// (category, zone) pair the recipe graph never had.
struct DeclaredProduct {
	uint64_t category = 0;
	std::string category_name;
	uint64_t location = 0;
	std::string location_name;
	std::string in_function;   // the function that declares it
	bool possible = false;     // SetPossibleOperationInfo: POSSIBLE product
};

// A declared summon procedure (`Fusion.AddProcMixN`, `Xyz.AddProcedure`, ...).
// THIS IS THE RECIPE, in machine form, and it beats seeding from the card TEXT
// on two counts: it carries codes rather than names to re-resolve, and it never
// gets the language wrong.
struct DeclaredRecipe {
	std::string proc;                                    // "Fusion.AddProcMixN"
	std::vector<std::pair<uint32_t, uint32_t>> named;     // (code, count)
	std::vector<std::pair<uint64_t, uint32_t>> setcode;   // (setcode, count)
	std::vector<uint32_t> unresolved_counts;             // unnamed cardinal
	bool must_be_fusion_summoned = false;
	int line = 0;
};

// A DECLARED STRING, and it is what carries the identity of the prompts.
//
// THE POINT THE HARNESS ALMOST MISSED. The decision that opens access to the
// graveyard is a `Duel.SelectYesNo(tp, aux.Stringid(id, 2))`: its description is
// NOT an effect's `SetDescription`, it is a Stringid written inline in the body
// of an operation. A harness indexing only `SetDescription` would declare the
// COMBO'S PIVOT "unmatched", i.e. it would return a false negative on the only
// decision that matters.
//
// So we index EVERY `aux.Stringid(id, n)` of the script, along with where it
// appears. `site` says which: an activation prompt (SetDescription) does not
// have the same checkable preconditions as a prompt raised mid-resolution.
struct DeclaredString {
	uint64_t value = 0;        // id * 16 + n, as the message carries it
	uint32_t card = 0;
	uint32_t index = 0;
	std::string in_function;
	std::string site;          // "SetDescription", "Duel.SelectYesNo", ...
	std::string effect_var;    // non-empty when site == "SetDescription"
	int line = 0;
};

struct CardOperators {
	uint32_t code = 0;
	std::string script;          // resolved file name (c<code>.lua)
	bool script_found = false;
	std::vector<DeclaredEffect> operators;   // declared in initial_effect
	std::vector<DeclaredEffect> grants;      // created during resolution: the STATES
	std::vector<DeclaredString> strings;
	std::vector<DeclaredRecipe> recipes;
	// Game procedures called by `initial_effect` (`Pendulum.AddProcedure`,
	// `Fusion.RegisterSummonEff`, ...). This is where the operators the card's own
	// script does not declare live.
	std::vector<std::string> proc_calls;
	std::vector<DeclaredProduct> products;
	std::vector<uint32_t> listed_names;      // s.listed_names
	std::vector<uint64_t> listed_series;     // s.listed_series
	// Zones the functions of this effect mention, by function. It is what lets us
	// say "this operator's cost touches DECK|EXTRA" without evaluating the closure.
	// evaluer la fermeture.
	std::unordered_map<std::string, uint64_t> fn_locations;
	// `Duel.<Verb>` verbs called, by function: the declared CONSUMPTION.
	std::unordered_map<std::string, std::vector<std::string>> fn_verbs;
	// Archetypes and codes NAMED by the function (`IsSetCard`, `IsCode`). Together
	// with `fn_locations`, this is what gives a negative edge a PLACE: "a Lunalight
	// from the EXTRA" instead of "something in the extra". Without them the
	// negative column of the balance sheet would have to be invented.
	std::unordered_map<std::string, std::vector<uint64_t>> fn_setcodes;
	std::unordered_map<std::string, std::vector<uint32_t>> fn_codes;
	// TYPES and RACES mentioned by the function (`IsType(TYPE_SYNCHRO)`,
	// `IsRace(RACE_DRAGON)`): the vocabulary of filters that name neither a code
	// nor an archetype. Without it, "summon 1 Synchro Dragon" has no readable
	// candidate and the precise card a reviver puts back in play is a place with no
	// producer (measured: 22 states with h = INFINITE in the middle of benchmark
	// B's real line).
	std::unordered_map<std::string, uint64_t> fn_types;
	std::unordered_map<std::string, uint64_t> fn_races;
	// Local functions called by a function. The constraint almost never lives in
	// the function that destroys: it lives in the FILTER that function passes to
	// `SelectMatchingCard`. One level is enough on this deck.
	std::unordered_map<std::string, std::vector<std::string>> fn_refs;
};

// SYMBOLIC LOCATIONS. `SetRange(LOCATION_PZONE)` is 0x200, but the core's
// message carries the PHYSICAL zone: a Pendulum Zone is a particular sequence
// of `LOCATION_SZONE` (0x8). Comparing the two without normalising would return
// "precondition VIOLATED" on every Wolf activation, a false negative of the
// harness, and exactly the same confusion that makes `--canonical-zones`
// dangerous.
//
// The mapping is the game's, not a heuristic: FZONE, PZONE and STZONE are spell
// zones; MMZONE and EMZONE are monster zones.
uint64_t NormalizeRange(uint64_t range);

class OperatorTable {
public:
	// `codes`: the cards to read (hand + extra + every watched code). Returns the
	// number of scripts actually read.
	size_t Build(const CardDB& db, ScriptProvider& sp, const ConstantTable& kt,
				 const std::vector<uint32_t>& codes);

	const CardOperators* Find(uint32_t code) const;

	// THE STRING THAT CARRIES THIS DESCRIPTION. This is the harness's matching: the
	// message carries (code, desc), and `desc = id * 16 + n` designates one and only
	// one `aux.Stringid(id, n)` of the deck. Returns null when none matches.
	const DeclaredString* ByDesc(uint64_t desc) const;
	// The effect named by a string whose site is `SetDescription` (null otherwise).
	const DeclaredEffect* EffectOf(const DeclaredString& s) const;
	// Fallback when the description is not a Stringid (a system string): the only
	// ACTIVABLE operator of this card that has no description of its own. Returns
	// null as soon as there is ambiguity; we do not guess.
	const DeclaredEffect* SoleUndescribed(uint32_t code, uint64_t act_mask) const;
	// EFFECT_TYPE_* mask of the effects a player can EMPLOY (operators in the
	// strict sense). Composed from `constant.lua`: no hard-coded value.
	static uint64_t ActivatableMask(const ConstantTable& kt);

	void Print(const CardDB& db, const ConstantTable& kt) const;
	// The census that picked out the work: which cards grant a STATE, and which
	// one. This is the line that returns the two pivots.
	void PrintGrants(const CardDB& db, const ConstantTable& kt) const;

	// THE NEGATIVE COLUMN: what an operator DESTROYS.
	//
	// The graph could say what an operator REQUIRES; it could not say what it
	// DESTROYS. The material was already extracted (`fn_verbs`, `fn_locations`,
	// `count_limit`) and read by NOBODY. This report reads it.
	//
	// IT IS NOT AN IMPROVEMENT, IT IS A CORRECTNESS CONDITION: TWELVE acquisition
	// edges out of thirteen described routes that DESTROY the goal, and the
	// criterion that removed them was a stopgap. The real criterion is consumption,
	// and it can be computed.
	//
	// `goal_zone` is the zone whose balance we want (LOCATION_EXTRA for a goal in
	// extra deck monsters): the final synthesis only keeps the edges that consume
	// IN that zone. That is the `A_p` line of the balance equation, the one whose
	// feasibility decides that a rollout is dead.
	void PrintConsumption(const CardDB& db, const ConstantTable& kt,
						  uint64_t goal_zone) const;

	// STEP 1: FIRING COUNTS, CONFRONTED WITH CAPACITIES.
	//
	// The balance equation returns three things; only the third is a heuristic. The
	// first two are here: the vector `x` (which operator, and HOW MANY TIMES) and
	// FEASIBILITY (`x_o <= cap_o`?). A goal with three identical copies does not ask
	// for "Liger's recipe": it asks that the summon fire THREE TIMES, and that each
	// requirement be served three times. That is the multiplicity `RecipeDistance`
	// never carried (2 "Name" is posed there as two requirements of one copy) and
	// that the operator bias cannot designate, since it only names ONE card.
	//
	// SCOPE, STATED UP FRONT. This is an AND expansion, through the DECLARED
	// recipe: exact when the recipe is unique (Liger's case), and only NECESSARY
	// when several routes exist. We do not choose in the game's stead: we expand
	// the declared route and say so. Neither ordering nor legality enters into it,
	// since conditions are closures.
	//
	// `deck` carries the codes WITH their duplicates (the number of copies is what
	// decides a "per COPY" capacity); `goal` is (code, copies).
	void PrintFiringCounts(
		const CardDB& db, const ConstantTable& kt,
		const std::vector<uint32_t>& deck,
		const std::vector<std::pair<uint32_t, uint32_t>>& goal) const;

	size_t Cards() const { return cards.size(); }
	size_t Missing() const { return missing; }
	size_t OperatorCount() const;
	size_t GrantCount() const;
	size_t ProductCount() const;

	const std::unordered_map<uint32_t, CardOperators>& All() const { return cards; }

private:
	std::unordered_map<uint32_t, CardOperators> cards;
	// full desc -> (card code, index into `strings`)
	std::unordered_map<uint64_t, std::pair<uint32_t, size_t>> by_desc;
	size_t missing = 0;
};

// --- THE OPERATOR PROGRAM ----------------------------------------------------
//
//     h(s) = min c'x   s.t.  A'x >= M_G - M_s ,  0 <= x <= u
//
// Four properties are proved in the design notes: admissibility (th. 1),
// consistency and hence a gradient (th. 2), dead ends proved by infeasibility
// (th. 3), and free tightening by any constraint every plan satisfies (th. 4).
//
// None of the four holds if the SOLVER lies. That is the only non-provable
// point, and it is treated as such: `LPResult` carries its own guards, checked
// on every call, and the module self-tests on instances with known solutions
// before it serves.
struct OperatorLP {
	size_t n_ops = 0;
	std::vector<double> cost;    // c
	std::vector<double> upper;   // u; kNoBound = unbounded
	struct Row {                 // sum coef.x >= rhs
		std::vector<std::pair<size_t, double>> coef;
		double rhs = 0.0;
		std::string label;
	};
	std::vector<Row> rows;
	static constexpr double kNoBound = 1e18;
};

struct LPResult {
	bool feasible = false;
	double value = 0.0;
	std::vector<double> x;
	// GUARDS. `primal_ok` checks A'x >= b and 0 <= x <= u on the returned
	// solution; `optimal_ok` checks that no reduced cost is negative any more. A
	// `false` here invalidates the measurement BEFORE it is used, instead of
	// letting it pass for a "slightly optimistic" heuristic.
	bool primal_ok = false;
	bool optimal_ok = false;
	double worst_violation = 0.0;
	// THE DUALS. They are already in the final tableau, so exposing them costs
	// nothing, and they replace CHOICES made by hand:
	//   row_dual[i]  : marginal value of constraint i (>= 0 at the optimum);
	//   bound_dual[j]: value of one extra unit of capacity on operator j
	//                  (> 0 <=> bound u_j BINDS the plan, i.e. its host is a host
	//                  with a COMPUTED quota, not a selected one).
	// Sign convention derived in tools/s22_verify_duals.py: in the normalised
	// tableau, a row dual is the reduced cost of its surplus/slack, and a bound
	// dual is the reduced cost of the bound slack.
	std::vector<double> row_dual;
	std::vector<double> bound_dual;
	// PHASE 1 FAILED: the constraints whose artificial stays positive, i.e. the
	// exact diagnosis of "which row is unsatisfiable HERE". Without this field,
	// "h = INFINITE" in the middle of a real line is mute (22 infeasible states on
	// benchmark B, with no visible cause).
	std::vector<std::string> infeasible_rows;
	// LIVENESS OF THE PATH QUOTAS (red-black relaxation). How many quota rows were
	// POSTED in this instance, and which hosts the asymmetry guard IGNORED, WITH
	// THE REASON, because the two cases call for opposite fixes: `unbounded` (the
	// host carries an effect with no declared bound, so the observed activation may
	// be that one and the aggregate would be a lie) is fixed by EXTRACTING the
	// bounds; `overrun` (observations > declared budget, i.e. the model does not
	// describe this host) is fixed by coverage. A mechanism that does not report
	// its own liveness has already cost two sessions of measurements on nothing.
	uint32_t quota_applied = 0;
	std::vector<uint32_t> quota_unbounded_hosts;
	std::vector<uint32_t> quota_overrun_hosts;
};

LPResult SolveOperatorLP(const OperatorLP& lp);

// Assembles `A`, `b` and `u` from the table, the deck and the goal, then prints
// `h` and the firing vector `x`. Returns `h` (or -1 when infeasible, th. 3).
//
// THREE ABSTRACT ZONES, AND NOT ONE MORE. `RESERVE` (deck + extra), `AVAILABLE`
// (hand, field, graveyard, banished: everything that can serve as material) and
// `FIELD` (where the goal is read). One more zone would be one more hypothesis
// to justify.
//
// THE RULE THAT GOVERNS EVERY MODELLING CHOICE: when in doubt, UNDER-CONSTRAIN.
// Under-constraining keeps `h <= h*` (theorem 1 holds, so `h = infinite` stays
// a PROOF of a dead end); over-constraining would make `h` larger than the true
// cost and turn the proof into a lie.
class BalanceModel {
public:
	// `goal`: FINAL state demands, by code, read @FIELD.
	// `transient`: PASSAGE demands, by code, read @AVAILABLE, i.e. "this card must
	// EXIST outside the reserve at some point in the line". This is how resolution
	// requirements (--resolve) are compiled into the balance sheet: the goal is
	// COMPILED, not rewarded. A transient demand is worth 1 per code (N resolutions
	// do NOT require N bodies, since the same body coming back is legal, so
	// requiring 1 is weaker than the truth: h stays admissible, per the
	// under-constraint rule). Asymmetry guard: a code with no readable producer is
	// NOT posted (an extraction gap must never become a false proof of
	// impossibility), and it is named.
	bool Build(const OperatorTable& tbl, const CardDB& db,
			   const ConstantTable& kt, const std::vector<uint32_t>& deck,
			   const std::vector<std::pair<uint32_t, uint32_t>>& goal,
			   const std::vector<std::pair<uint32_t, uint32_t>>& transient = {});
	// `res` (deck+extra), `ava` (hand, field, graveyard, banished) and `fld`
	// (monster zone) are the PHYSICAL codes present in each zone in the current
	// state. Returns h, or -1 when infeasible (theorem 3).
	//
	// `spent`: OBSERVED uses of quota hosts along the path leading to this state
	// (canonical code -> MSG_CHAINING activations). This is the RED-BLACK partial
	// relaxation (Katz-Hoffmann-Domshlak) of the balance sheet: capacities become
	// the PATH's, not the start's. An activation bit does not say WHICH of the
	// host's effects fired, so the row posted is the AGGREGATE
	// Sigma x_t <= (Sigma u_t) - s over all the host's transitions, which is safe
	// under any attribution (Sigma s_t >= s). Asymmetry guards, named in LPResult:
	// a host one of whose transitions has no declared bound -> ignored (the
	// observed activation may be the unbounded effect); an observation beyond the
	// declared budget -> ignored (the model does not describe this host, and
	// applying it would prove false deaths).
	double Solve(const std::vector<uint32_t>& res,
				 const std::vector<uint32_t>& ava,
				 const std::vector<uint32_t>& fld,
				 LPResult* out = nullptr,
				 const std::vector<std::pair<uint32_t, uint32_t>>& spent =
					 {}) const;
	// THE SUBGOALS, DERIVED FROM x*. Every firing transition produces places: those
	// are the steps every optimal plan of the program has to cross. This is the
	// SERIALISATION, computed rather than guessed, and it carries the INTERMEDIATE
	// places (a body available, a code acquired), where `CommonCodes` only counts
	// the target cards already PLACED and therefore stays flat over the whole climb
	// (85 % of states are mute).
	struct Need {
		uint32_t code = 0;    // 0 when the requirement is an archetype
		uint64_t arch = 0;
		uint8_t zone = 0;     // 0 RESERVE, 1 AVAILABLE, 2 FIELD, 3 GRAVEYARD
		uint32_t count = 1;
	};
	std::vector<Need> NeedsFrom(const LPResult& r) const;
	// THE CONSUMPTION RUNGS. The profile of the gaps showed three deserts of 46 to
	// 73 answers along the real line: all the ASSEMBLY work (renames, fusions) is
	// invisible there because its products fall back into aggregate places that are
	// already saturated, and the closed form (cost ~ Sigma b^(l_i), dominated by
	// b^(l_max)) says those deserts alone rule out a bare run. But every consuming
	// firing SENDS a body to the graveyard, and that arrival climbs STEADILY during
	// the deserts. So we return the NEGATIVE column of x* as @GRAVEYARD subgoals:
	// "how many of the required firings have happened", read from the state. It is
	// the progress criterion the design notes named without having built it.
	// Over-counting is HARMLESS (a unit never reached creates no cell);
	// under-counting would leave the deserts whole.
	std::vector<Need> ConsumedFrom(const LPResult& r) const;
	// THE QUOTA HOSTS, DERIVED FROM THE DUALS. Replaces a by-hand derivation
	// (effect classes + crossed series, three iterated versions, until Wolf
	// appeared). A host enters here by COMPUTATION:
	//   - its transition has a declared FINITE bound, and
	//   - it is fired by x* (x_t > 0), saturated (x_t = u_t), with a positive dual
	//     (the capacity BINDS the plan), or produces a place x* consumes
	//     (robustness to degeneracy: three zero-cost igniters are interchangeable
	//     for the simplex, not for the path).
	// `presence` receives the hosts of the transitions FIRED by x*: the enablers,
	// from which the IN-PLAY rungs are derived.
	std::vector<uint32_t> QuotaHostsFrom(const LPResult& r,
										 std::vector<uint32_t>* presence) const;

	size_t Places() const { return pname.size(); }
	size_t Transitions() const { return lp.n_ops; }
	size_t Renames() const { return n_rename; }
	// Fusion igniters read. At zero while the deck has Fusion recipes, ignition
	// coupling is DISARMED (asymmetry guard); saying so is the mechanism's
	// liveness.
	size_t FusionIgniters() const { return n_igniter; }
	const std::vector<std::string>& TrNames() const { return tname; }
	const std::vector<std::string>& PlaceNames() const { return pname; }
	// The HOST of each transition: the card carrying the effect. It is the key that
	// turns a bound dual into a QUOTA HOST, the computed derivation that replaces
	// hand-written rules.
	uint32_t HostOf(size_t t) const {
		return t < thost.size() ? thost[t] : 0;
	}
	// The declared upper bound of each transition (kNoBound = none).
	double UpperOf(size_t t) const {
		return t < lp.upper.size() ? lp.upper[t] : OperatorLP::kNoBound;
	}
	// Name of the first place PRODUCED by this transition (readability).
	std::string Produces(size_t t) const;

private:
	size_t PlaceId(int kind, uint64_t key, int zone) const;
	std::vector<uint64_t> SetcodesOf(uint32_t code) const;
	const CardDB* db = nullptr;
	std::unordered_map<uint64_t, size_t> pid;
	std::vector<std::string> pname;
	std::vector<std::string> tname;
	std::vector<uint32_t> thost;   // host (card) of each transition, 0 when n/a
	std::vector<double> need;                       // goal per place
	std::vector<std::unordered_map<size_t, double>> col;   // effects per transition
	OperatorLP lp;                                  // costs and bounds; rows rebuilt
	// Canonical codes of the GOAL: the consumption rungs must never bear on them
	// (the ladder used to reward sending the LIGERS to the graveyard, an artefact
	// of the destruction cost in x*, and the bare run archived cells "in progress"
	// that had destroyed their own goal pieces).
	std::vector<uint32_t> goal_codes;
	size_t n_rename = 0;
	size_t n_igniter = 0;
	// The cache is MUTABLE under const: workers call Solve() in parallel
	// (RefineLadderHere), so the lock is mandatory.
	mutable std::mutex sc_mx;
	mutable std::unordered_map<uint32_t, std::vector<uint64_t>> sc_cache;
};

double BuildAndSolveBalance(
	const OperatorTable& tbl, const CardDB& db, const ConstantTable& kt,
	const std::vector<uint32_t>& deck,
	const std::vector<std::pair<uint32_t, uint32_t>>& goal,
	const std::vector<std::pair<uint32_t, uint32_t>>& transient = {});

// Returns the number of cases passed over the number of cases. Must be n/n.
size_t SelfTestOperatorLP(size_t* total);

// Extraction of ONE already-read script. Exposed for the test: it touches
// neither the disk nor the database.
//
// `init_fn` names the function whose effects are OPERATORS. That is
// `initial_effect` for a card script; for a `proc_*.lua` file it is the
// procedure called (`Pendulum.AddProcedure`), and the effects it registers then
// belong to the calling card.
CardOperators ParseScript(uint32_t code, const std::vector<char>& src,
						  const ConstantTable& kt,
						  const std::string& init_fn = "initial_effect");

// --- THE MISSING NODE TYPE ---------------------------------------------------
//
// The recipe graph files `Lunalight Leo Dancer` as a PRODUCT TO BUILD, hence
// `--backward` and its "2 subproducts, 0.02 built": it was trying to build a
// non-buildable card, since its named material is absent from the deck.
//
// But Leo is not a product here: it is an ACQUIRABLE PROPERTY. `Kaleido Chick`
// grants `EFFECT_ADD_CODE`, the code of the card it sends to the graveyard,
// valid as FUSION MATERIAL. The move IS in the space; it takes a two-step
// preparation, and nothing in the graph could express that.
//
// So an acquisition edge is: "this CODE is obtained if the HOST is in its zone
// and a SOURCE carrying that code is in the zone the operator reaches". Two
// requirements, no manufacturing.
struct AcquirableCode {
	uint32_t code = 0;         // the acquired code
	uint32_t host = 0;         // the card carrying the operator
	uint64_t host_range = 0;   // where it must be (LOCATION_*, already normalised)
	uint64_t source_zone = 0;  // where the source must be
	std::string grant;         // EFFECT_ADD_CODE / EFFECT_CHANGE_CODE
};

// `owned`: the cards the deck owns (hand + extra). The edge is only posted
// towards codes that are REALLY available; an acquisition towards an absent
// code would be a dead route.
std::vector<AcquirableCode> AcquirableCodesOf(const OperatorTable& tbl,
											  const CardDB& db,
											  const std::vector<uint32_t>& owned);

// --- THE HARNESS: CONFRONTING THE TABLE WITH THE RESOLVED PLAN ---------------
//
// One activation recorded during the replay. The message decoder returns it;
// `operators.cpp` decodes nothing itself.
struct ObservedActivation {
	size_t at = 0;            // answer index
	uint8_t message = 0;      // MSG_SELECT_IDLECMD, _CHAIN, _EFFECTYN, ...
	uint32_t code = 0;        // card engaged (canonical)
	uint64_t desc = 0;
	uint8_t location = 0;     // PHYSICAL zone the card activates from
	uint32_t sequence = 0;
	int turn = 0;
};

struct HarnessVerdict {
	size_t total = 0;
	// The description is an `aux.Stringid` of a card in the deck: it designates ONE
	// operator and one only. This is the strongest case.
	size_t matched_by_desc = 0;
	// The description is a SYSTEM string the operator declares itself
	// (`SetDescription(1160)` of a procedure): the match stays exact.
	size_t matched_by_system = 0;
	// The description is manufactured by the CORE (`processor.cpp` emits 221 or 0
	// for "activate this card's trigger effect?"). The message then does NOT say
	// which effect: only the card identifies the operator. So this counter
	// measures, in negative, what the (code, description) identity does NOT
	// separate, and that is a property of the protocol, not of the extraction.
	size_t matched_by_card = 0;
	// ... and among those, the ones where several operators stay possible even
	// after the zone. The harness does not decide: it counts.
	size_t ambiguous = 0;
	size_t unmatched = 0;            // NO declared operator: the table lies
	size_t no_script = 0;            // card with no script read (outside the table)
	size_t zone_checked = 0, zone_ok = 0, zone_violated = 0;
	size_t count_checked = 0, count_violated = 0;
	std::vector<std::string> failures;   // the detail, bounded
};

HarnessVerdict ConfrontPlan(const OperatorTable& tbl, const CardDB& db,
							const ConstantTable& kt,
							const std::vector<ObservedActivation>& acts);
void PrintVerdict(const HarnessVerdict& v);

} // namespace solver
