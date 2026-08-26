// Search over the duel's state graph.
//
// The action tree is not enumerable at any speed (10^97 along the reference line
// alone). What makes exploration possible is searching the STATE GRAPH:
// activating A then B and B then A converge on the same node, and the
// transposition table merges them.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "duel.h"
#include "enumerate.h"
#include "prompt.h"
#include "replay.h"

namespace solver {

// Target board in the sense of the equivalence criterion we use: same cards per
// zone TYPE, same materials, same counters, same FACE (up/down). The exact
// column is ignored, and so is the ATK/DEF battle position (two boards that
// differ only by a battle position are the same board). The STATE digest, on
// the other hand, keeps the full position.
struct BoardKey {
	uint64_t hash = 0;
	std::vector<uint64_t> entries;   // sorted, for an exact comparison
	// LOOSE entries: (zone, code, face) and nothing else, neither materials nor
	// counters. Sorted. This is the comparison used for targets POSTED by hand
	// (`--target`), which are a code and a position: a posted Xyz has no materials,
	// so it would NEVER equal a real Xyz, which always carries some.
	std::vector<uint64_t> loose;
	// Codes alone, sorted. Much coarser than `entries`, but that is exactly what is
	// needed to guide: a placed card counts as soon as it is there, without waiting
	// for its materials and its final position.
	std::vector<uint32_t> codes;
	// Monsters present, captured along the way: avoids one extra zone query per
	// heuristic evaluation (a measured hot spot).
	uint32_t mzone_count = 0;
	bool operator==(const BoardKey& o) const { return entries == o.entries; }
};

BoardKey ComputeBoardKey(Duel& duel, uint8_t con);
// Variant reusing `out`: two zone queries per decision, tens of millions of
// times per run, and allocating per call was a hot spot.
void ComputeBoardKeyInto(Duel& duel, uint8_t con, BoardKey& out);

// Builds a BoardKey from EXPLICIT card lists. Editing the target board (drop
// Hot Red, require Naturia Beast) goes through here: we start from the cards
// captured on the reference board, remove, add, and recompose the key exactly
// as ComputeBoardKey would have.
BoardKey MakeBoardKey(const std::vector<QueriedCard>& mzone,
					  const std::vector<QueriedCard>& szone, const CardDB& db);

// Cardinality of the intersection of the sorted codes: how many cards of the
// target board are placed. This is the "number of subgoals reached" that
// serialises the conjunctive goal.
uint32_t CommonCodes(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b);

// --- Novelty pruning (Iterated Width) ------------------------------------
//
// The transposition table only merges IDENTICAL states; yet two lines differing
// by one card in the graveyard are distinct and still not distinctly
// interesting. Novelty pruning inverts the criterion: a state is kept only if
// it makes at least one ATOM true, an atom being a fact (zone, card,
// occurrence) of the target player, that is unseen or reached earlier than ever
// before. The cost becomes exponential in the WIDTH of the problem, not in the
// size of the space (Lipovetzky & Geffner; Rollout-IW, arXiv:1801.03354).

// Atoms of a state: field entries (fine-grained and by code), contents of the
// hidden zones with occurrence, deck count. `partition` is mixed into every
// atom: partitioning by subgoal reached reopens the table at every target card
// placed, which is the serialisation of the goal (Serialized IW / BFWS).
void CollectAtoms(Duel& duel, uint8_t con, const BoardKey& here,
				  uint32_t partition, std::vector<uint64_t>& out);

// --- Board guard (Nibiru windows) ----------------------------------------
//
// A clause is a conjunction of conditions (card present in one of the zones of
// the mask). The guard holds when AT LEAST ONE clause holds entirely. Example:
// "Crystal Wing@field" OR "Zalen@field + Junk Signal@hand".
struct GuardAtom {
	uint32_t code = 0;    // canonical; 0 when the atom is a PREDICATE
	uint32_t zones = 0;   // LOCATION_* mask
	// State PREDICATE (a game rule stated by the player): instead of a
	// card-in-zone, the atom requires a COUNT. kind 0 = card@zone
	// (historical); kind 1 = "oppbanished>=count" (OPPONENT cards
	// banished). Use: "Dis Pater@field + oppbanished>=1" is an available
	// negation, on the same footing as Crystal Wing.
	uint8_t kind = 0;
	uint32_t count = 0;
};
using GuardClause = std::vector<GuardAtom>;

// Does the guard hold in the current state? `field_codes`: sorted codes of the
// player's MZONE+SZONE cards (BoardKey's; ocgcore's field zone is slot 5 of the
// SZONE, so the field spell is covered). The other zones are only queried when
// a clause mentions them.
bool GuardHolds(Duel& duel, uint8_t con, const std::vector<GuardClause>& clauses,
				const std::vector<uint32_t>& field_codes);

// Minimum number of effect resolutions (--resolve "card[@zone][:n]"). `zones`
// restricts the ACTIVATION zone: the Omega that rips the hand activates from
// the FIELD, and counting its graveyard effect was a false positive that a
// whole approach was judged on. 0 = every zone.
struct ResolveReq {
	uint32_t code = 0;        // canonical
	uint32_t min_count = 1;
	uint32_t zones = 0;       // LOCATION_ mask of the activation zone
	// --summon-min: count the SUMMONS (MSG_SUMMONING/SPSUMMONING) of the card
	// instead of its activations. Same mechanism end to end (goal gate,
	// gradient, bias, histogram). The measured use case: Junk Meister summons
	// itself through its own effect (revealing Stardust/Accel/a third Synchro),
	// so its PLACEMENT is the rare event that locks Naturia Beast in, not an
	// activation.
	bool on_summon = false;
};

// ACTIVATION zone of a MSG_CHAINING: triggering_location. Payload
// (processor.cpp:3700): code u32, loc_info (controler u8, location u8,
// sequence u32, position u32), triggering_controler u8, triggering_location u8
// at offset 15. 0 when the message is too short.
inline uint32_t ChainingLocation(const uint8_t* data, uint32_t size) {
	return size >= 16 ? data[15] : 0u;
}

class NoveltyTable {
public:
	// Returns true when at least one atom is unseen, or, in non-strict mode, seen
	// only deeper, and updates the table.
	//
	// Two regimes, chosen on MEASUREMENT rather than by taste:
	// - non-strict (depth-sensitive): reaching a fact again in FEWER decisions
	//   counts as new. That is the safe regime for same-deck repair (it loses no
	//   solution there), but a DFS visiting the deep deviations first makes any
	//   shorter branch "new": 3 584 cuts over 1 M states at one deviation, for
	//   nothing.
	// - strict: only a fact NEVER seen counts. This is the IW pruning of the
	//   literature, reserved for passes looking for new material.
	bool Observe(const std::vector<uint64_t>& atoms, uint32_t depth,
				 bool strict = false) {
		bool novel = false;
		for(uint64_t a : atoms) {
			auto [it, fresh] = seen.emplace(a, depth);
			if(fresh) {
				novel = true;
			} else if(!strict && depth < it->second) {
				it->second = depth;
				novel = true;
			}
		}
		return novel;
	}
	// Variant with no update, to count without consuming.
	bool WouldBeNovel(const std::vector<uint64_t>& atoms, uint32_t depth) const {
		for(uint64_t a : atoms) {
			auto it = seen.find(a);
			if(it == seen.end() || depth < it->second)
				return true;
		}
		return false;
	}
	void Clear() { seen.clear(); }
	size_t Size() const { return seen.size(); }

private:
	std::unordered_map<uint64_t, uint32_t> seen;   // atom -> min depth
};

// State fingerprint used as the transposition key. The zones give the visible
// part; the prompt payload gives most of the invisible part, i.e. which effects
// are still activable, hence the once-per-turn counters the public API does not
// expose.
//
// Under-hashing merges distinct states and makes solutions DISAPPEAR without
// saying so: that is the failure mode to watch.
// `sort_field`: conflate the field's COLUMNS.
//
// WHAT THAT FIXES, measured before it was written. At the stable points (idle
// prompt), the key takes 7 033 distinct values for 51 boards. Attribution,
// component by component:
//     board (BoardKey)                    51   x1.0
//     + game state, columns conflated    151   x3.0    <- legitimate (materials)
//     + the COLUMN distinguishes        5088   x99.8   <- x33.7 for it alone
//     + prompt payload                  7033   x137.9  <- x1.38
//     + processor state                 7033   x137.9  <- x1.00 (empty stack here)
// The column is by far the biggest contributor, and it is the only one that is
// NOISE: `BoardKey` explicitly ignores it, the goal criterion ignores it, and
// two monsters swapping columns carry out the same intent.
//
// WHY IT IS OPT-IN ANYWAY. LINK ARROWS point at columns, and a pointed zone
// allows a summon from the extra deck, so conflating columns can merge two
// states that are NOT equivalent and silently DELETE a solution, the failure
// mode this project fears most. So the caller has to enable it knowingly, and
// `main` warns when the deck contains a Link monster.
uint64_t StateDigest(Duel& duel, uint8_t prompt_type,
					 const std::vector<uint8_t>& prompt_payload,
					 bool sort_field = false);

// COMPONENTS of the transposition key, for ATTRIBUTION. `full` is EXACTLY what
// StateDigest returns; the split changes no value, it only exposes the
// intermediate steps:
//   zones        : the six zones of both players + deck counts. The HIDDEN
//                  zones are already canonicalised by sorting.
//   with_payload : zones + prompt type + prompt payload.
//   procstate    : the processor state ALONE (resolution stack).
//   full         : with_payload + procstate.
// The number of DISTINCT values of each, counted at the same nodes, says which
// one blows the table up, and by how much.
struct DigestParts {
	uint64_t zones = 0;
	// The SAME thing, but with the field SORTED instead of ordered: the column
	// stops distinguishing. `StateDigest` preserves the order, since the column can
	// matter (link arrows, column-dependent effects), a CONSERVATIVE choice that
	// `BoardKey` does not make on its side. The gap between `zones` and
	// `zones_sorted` IS the price of that choice, and it is the only figure that
	// says whether a fix is needed.
	uint64_t zones_sorted = 0;
	uint64_t with_payload = 0;
	uint64_t procstate = 0;
	uint64_t full = 0;
};
DigestParts StateDigestParts(Duel& duel, uint8_t prompt_type,
							 const std::vector<uint8_t>& prompt_payload);

// One step of the reference line, expressed in CARDS rather than in indices.
//
// A recorded answer says "the third element of the list". In another duel the
// list has neither the same contents nor the same order: replaying the bytes
// means nothing. The edge, on the other hand, is built on card codes and on the
// nature of the choice, so it survives a change of deck, hand and seed.
struct PlanStep {
	uint8_t prompt_type = 0;
	uint64_t edge = 0;     // 0 = step not identified, unusable as a guide
	std::string label;
};

// Records the reference line in semantic form.
//
// The recorded answer is not comparable byte for byte with what the solver
// enumerates (EDOPro encodes its selections as bitsets, the enumerator as index
// lists). So each decision is identified by the STATE it reaches: apply,
// compare, restore, the same test as the coverage check, which the arena makes
// affordable.
//
// The duel must be at the start; it is left at the end of the line. Returns the
// number of unidentified steps, which are that many holes in the guide.
size_t LiftPlan(Duel& duel, Arena& arena, const Replay& yrp, int target_player,
				size_t stop_after, const EnumOptions& eo,
				std::vector<PlanStep>& out);

// Records the reference line for REPAIR, indexed by answer (yrp.responses, both
// players):
// - `digests`: digest of the state BEFORE each answer -> index. During
//   DescendRepair, a state whose digest is that of a point FURTHER ALONG the
//   reference picks the line up there: the recorded suffix becomes readable
//   again after a deviation that converges.
// - `keys`: plan_key of the move played at each index (0 when unidentified or
//   when it is an opponent decision), i.e. LiftPlan's matching, here by answer
//   index. It is repair's WINDOWED repertoire: after a first deviation,
//   replaying a reference move near the current point is free, and a local
//   permutation (swapping summons #4/#5, a measured ~17 decisions apart) costs
//   ONE deviation instead of one per decision.
// The duel must be at the start; it is left where the line stops.
//
// `stats` (optional): per PLAYER decision, the enumerated arity and whether the
// reference's move was FOUND among the choices. Those two numbers answer two
// questions that "N/M moves identified" has always CONFLATED:
//
//   - `matched` = is the move played IN the solver's action space? That is
//     COVERAGE, and a single "no" is enough to make the line unreachable at any
//     budget.
//   - `keys[i] != 0` = is that move in the REPERTOIRE? That is another thing,
//     and a move found but without a plan_key must not count as unidentified.
//
// The arity gives the likelihood: with a NEW policy every logit is equal (the
// weights start at zero), so the probability that a uniform rollout reproduces
// the line is exactly the product of the inverse arities. It is the only figure
// that says whether sampling has a chance.
struct RefLineStats {
	std::vector<uint32_t> arity;    // 0 = opponent decision (not enumerated)
	std::vector<uint8_t> matched;   // 1 = reference move found
	// Prompt TYPE per decision. Without it, `log10 P` is a single number and does
	// not say WHERE the improbability goes: a POSITION decision cannot affect the
	// goal, an IDLECMD decision decides it. Breaking the 105 orders of magnitude
	// down by type separates two opposite lines of work (quotient against
	// serialisation) for the price of one byte per decision.
	std::vector<uint8_t> prompt;
	// THE DETAIL OF THE MISSES. A coverage hole one cannot READ cannot be fixed:
	// we keep the prompt, the REAL answer and the OFFERED answers, so that a
	// byte-for-byte comparison decides between "the move is not enumerated" and
	// "it is, but in another form" (the order of a selection, or a different
	// physical copy that `dedup_by_code` would have folded).
	// aurait repliee).
	struct Miss {
		size_t index = 0;
		uint8_t prompt = 0;
		std::vector<uint8_t> recorded;
		std::vector<std::vector<uint8_t>> offered;
	};
	std::vector<Miss> misses;
	static constexpr size_t kMaxMisses = 8;
};
void LiftRefLine(Duel& duel, Arena& arena, const Replay& yrp, int target_player,
				 size_t stop_after, const EnumOptions& eo,
				 std::unordered_map<uint64_t, size_t>& digests,
				 std::vector<uint64_t>& keys,
				 RefLineStats* stats = nullptr);

// --- NRPA policy: shared between workers ------------------------------------
//
// One weight per move code (plan_key), softmax sampling. A decision of a rollout
// under the policy is memorised for the adaptation; the biases (repertoire,
// hints) are kept, because the adaptation must recompute the SAME probabilities
// as the sampling.
struct PolicyStep {
	std::vector<uint64_t> keys;   // plan_key of each legal choice
	std::vector<uint8_t> known;   // in the repertoire?
	std::vector<uint8_t> hinted;  // engages a --hint card?
	size_t chosen = 0;
	// CONTEXT of the decision: number of target board cards already placed. A
	// SEMANTIC descriptor (not positional: two lines do not ask the same
	// questions at the same index) and available on both sides, at rollout time
	// as well as when recording a corpus line.
	uint16_t ctx = 0;
	// EFFECTIVE CONTEXT of the policy's contextual level. This is the key
	// actually passed to CtxKey; it equals `ctx` when the head bandit is off
	// (byte-for-byte the previous behaviour) and the PATH CONDITIONING when it
	// is on (cfg.mcps_depth > 0).
	//
	// Why two fields and not one. `ctx` stays the SEMANTIC descriptor: the
	// option guard decodes it into (placed, hand) through OptionCtxCompatible,
	// and ForecastSearchCost reads its high component to spot the sqrt-LTS hint
	// points. Overwriting `ctx` would break both readings; separating them
	// leaves each consumer on its own conditioning.
	uint64_t cctx = 0;
	// This decision was taken by the HEAD BANDIT (--qhat) and not sampled
	// under the policy. AdaptRun SKIPS it: the NRPA gradient is only valid for
	// a move DRAWN from the softmax, and applying it to a move chosen by argmax
	// would push it +alpha every time with no counterweight, which is exactly
	// the failure mode that killed the earlier path-conditioning attempt.
	// The bandit has its own memory (Q and Q^, averages over ALL rollouts);
	// that is what plays the gradient's role on those decisions.
	uint8_t bandit = 0;
};
struct NrpaRun {
	double score = -1;
	std::vector<PolicyStep> steps;
	// NUMBER OF STEPS AT WHICH THE SCORE REACHED ITS MAXIMUM.
	//
	// THE DEFECT THIS FIELD FIXES, in two lines of source. A rollout's score is a
	// MAX over prefixes (`if(sc > run.score)`), but `AdaptRun` walks EVERY step. A
	// line peaking at 7/8 on step 200 and then wandering for 230 more steps has all
	// 430 steps reinforced at +alpha: the policy learns the post-peak collapse
	// exactly as strongly as the climb.
	//
	// The signature is measured: benchmark B's failure regime writes an approach of
	// 434 decisions for a ceiling of 435, against 189-258 when it succeeds. The
	// rollout does not stop because it is done, it stops because it is spent, and
	// everything it did after its peak is learned.
	//
	// Always filled (cost: one assignment per improvement); CONSUMED only under
	// `--adapt-to-peak`, so that turning the flag moves one factor only.
	size_t peak_steps = 0;
	// FLAT line (online mining): the SAME line, but with all its multiple-choice
	// decisions in ATOMIC form, including those a macro absorbed, which produce no
	// `steps`. Filled only when `cfg.options_online` is wired; empty otherwise (no
	// cost).
	//
	// WHY it is indispensable to re-mining. A line found WITH macros records the
	// MACRO'S ID as the move played; re-mining on that would produce macros of
	// macros, whose second key is never offered by a prompt
	// (`choices[i].plan_key` is always atomic), i.e. a systematic abort on the
	// first step. The emergent hierarchy compression promises is therefore
	// obtained by RE-FLATTENING: a macro mined on `flat` can cover what a
	// previous macro absorbed, and come out longer than it. Same hierarchy,
	// realised on the miner's side rather than the executor's.
	std::vector<PolicyStep> flat;
};

// Best GLOBAL sequence, shared between the NRPA workers. Restarts used to
// forget the sublines learned (measured: 6/8 with the same cards missing run
// after run); here every restart starts again from the best line known to ALL
// the workers, and a stalling worker adopts it. Low-frequency mutex: one
// acquisition per upper-level iteration, not per rollout.
struct NrpaShared {
	std::mutex mu;
	NrpaRun best;
};

// NRPA policy: one weight per move code (plan_key). Public because it now
// SURVIVES the run: the weights the rollouts learn die with the run although
// they are exactly the guide the finisher needs. RunLevin consumes them, and
// the workers merge them (weight average).
using NrpaPolicy = std::unordered_map<uint64_t, float>;

// --- TWO-LEVEL policy -------------------------------------------------------
//
// ATTRIBUTION NOTE. This block is NOT the MCPS algorithm (arXiv:2510.06381),
// although it was once labelled as inspired by it. MCPS is an MCTS: a TREE,
// per-node visit counts, and three estimators that are REWARD AVERAGES,
// combined with weights proportional to the sample sizes. What follows combines
// two LOGITS updated by the NRPA gradient of the single best sequence. No tree,
// no visit count, no reward average: a convex combination of two logits is not
// a convex combination of two win rates, and the retention formula
// s = n/(n+k) below REINTRODUCES exactly the bias hyperparameter the paper
// removes. What this block borrows from MCPS is the IDEA of combining several
// estimators of one move weighted by their evidence, not its algorithm. The
// real MCPS lives further down, under PermWindow / BanditNode.
// Enriching the context key alone would trade a ceiling for a famine: each
// context cell would see a fraction of the updates. MCPS answers exactly that
// dilemma by COMBINING several estimators of one move instead of choosing one,
// weighted by their evidence. So we keep BOTH levels:
//
//     w_eff(move, ctx) = (1 - s) * w_global[move] + s * w_ctx[move, ctx]
//     s = n / (n + k)   where n = number of updates of the context cell
//
// Limits: n = 0 -> exactly w_global (no fragmentation penalty, the fresh cell
// says nothing); n >> k -> w_ctx (full state dependence); k < 0 -> mechanism
// OFF, bit-for-bit the previous behaviour.
//
// Why a calibrated retention rather than raw sample sizes (the point that makes
// this mechanism foreign to MCPS): the two levels are NESTED, the global one
// aggregates every context and its sample size always dominates, so a weighting
// by raw sample sizes would NEVER hand over to the contextual level. Hence the
// calibrated k. It is a convex combination of two logits of the SAME scale: it
// does not change the softmax temperature, so what it measures is state
// dependence and nothing else. But it is also, exactly, the bias hyperparameter
// the MCPS derivation eliminates, and that is why this block cannot claim the
// paper.
struct CtxWeight {
	float w = 0;
	uint32_t n = 0;
};
using NrpaResidual = std::unordered_map<uint64_t, CtxWeight>;

// The CONTEXT descriptor, computed identically at rollout time and when
// recording. Two axes, both semantic and cheap: how many target board cards are
// placed (0 -> 8, the combo builds up) and how many cards are left in hand
// (5 -> 0, the resources are spent). The first alone does not separate enough:
// measured on the corpus, 8 cells for ~165 decisions per line, and only +4.3
// points of agreement. The hand is the natural orthogonal axis, since two
// moments with the same board but a different hand do not ask the same
// questions.
inline uint16_t ContextKey(uint32_t placed, uint32_t hand) {
	if(placed > 15) placed = 15;
	if(hand > 15) hand = 15;
	return static_cast<uint16_t>(placed * 16u + hand);
}

inline uint64_t CtxKey(uint64_t key, uint64_t ctx) {
	return key ^ ((ctx + 1) * 0x9e3779b97f4a7c15ull);
}

// --- PATH CONDITIONING: WINS NOTHING, OFF BY DEFAULT ------------------------
//
// VERDICT: it loses twice over. (1) In search, zero comparisons won out of four
// against merely TURNING ON the contextual level with its older descriptor
// (`--ctx-shrink 8`), and a total collapse at k = 6 on one seed (best 0/4, 99 %
// of rollouts dead at the turn change, 27 distinct lines kept out of 8 751
// offered: basin lock-in). (2) In representation, the agreement curve gives the
// SAME plateau as the older conditioning (59 against 59).
//
// THE DIAGNOSIS, more important than the verdict: this flag grafted the MCPS
// CONDITIONING onto the WRONG OBJECT. The paper conditions a REWARD AVERAGE
// computed over every game, the bad ones included; here it conditions a LOGIT
// updated by the gradient of the single best sequence. The observed failure
// mode is exactly the one the missing half predicts: the gradient pushes +alpha
// with none of the counterweight an average over all games would bring, and
// each conditioned cell receives too few competing updates to recover. The
// paper's mechanism is implemented below, under PermWindow / BanditNode
// (--qhat); THIS flag is OFF by default and says so loudly when turned on.
//
// What follows describes what the flag DOES, not what should be done:
//
// COMMUTATIVE AND NOT SEQUENTIAL, for two reasons that coincide: MCPS talks
// about the games that CONTAIN the moves (a set, not a sequence), and our
// guiding principle is search over the state GRAPH, where activating A then B
// and B then A converge. A sum of mixed moves is therefore more faithful to
// both than a sequential hash. A sum and not a XOR: our lines replay the same
// move several times (three Kaleido Chick in the reference) and a XOR would
// cancel them pairwise.
inline uint64_t MixMove(uint64_t key) {
	uint64_t z = key + 0x9e3779b97f4a7c15ull;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

// --- PERMUTATION STATISTIC Q^ (MCPS, arXiv:2510.06381) -----------------------
//
// THE PAPER'S MECHANISM, for real. The path-conditioning attempt above took
// MCPS's CONDITIONING and put it on NRPA logits, which wins nothing. What
// follows takes the OBJECT: reward averages over sets of rollouts, combined
// with weights proportional to the sample sizes.
//
// WHAT MCPS DOES, in three lines. At a node s reached by the path (a0..ad), to
// evaluate a move a it averages the rewards of three sets of games: Q(s,a)
// those that START with the path then a, Q~(sr,a) GRAVE's AMAF at a reference
// ancestor, and Q^(sr,a), its contribution, ALL the games containing a and
// every move of sr, in any order and anywhere (Virtual Global Search). It
// combines them with val = (n Q + n~ Q~ + n^ Q^) / (n + n~ + n^), the
// minimum-variance convex combination under independence, and that is what
// REMOVES GRAVE's bias hyperparameter. Machinery: one bitset per move code over
// a sliding window of the last W rollouts, plus the parallel array of their
// rewards; Q^ is computed by popcount over the intersection.
//
// WHAT WE TAKE FROM IT, AND WHAT WE DO NOT.
//
// - Q^ and Q, yes. The bitset window carries over as is into an architecture
//   WITHOUT a tree: it depends only on the rollouts. Q needs a tree, so we
//   build one, tiny, over the first k decisions only (measured: 207 cells cover
//   the first six). It is a real bandit at the top of the tree, where branching
//   is narrow and where one opening decides the line's viability; past k, NRPA
//   samples as before, byte for byte.
// - Q~ (GRAVE's AMAF), NO, and for a substantive reason rather than laziness:
//   AT THE ROOT THE TWO ESTIMATORS COINCIDE. Q~(root,a) = average of the games
//   through the root that contain a = average of ALL the games containing a =
//   Q^(root,a), the path condition being empty. Our tree is six plies deep and
//   its permutation reference is the root or one of its very first descendants:
//   adding Q~ would count the same rollouts twice, which the minimum-variance
//   derivation (which assumes the estimators independent) does not forgive. So
//   we keep the pair (Q, Q^) and the SAME weight formula, proportional to the
//   sample sizes, with no bias hyperparameter.
//
// THE REWARD, chosen explicitly BEFORE coding (the paper works on win rates in
// [0,1]; our rollout score is material x 1000 + novelty, unbounded and
// incomparable from one run to the next). r = (best material reached along the
// rollout) / (material of the target board), capped at 1, and exactly 1 when
// the goal is reached. Three intended properties: bounded, comparable across
// runs (the denominator is a constant of the PROBLEM, not the current best
// score; normalising by that would move the scale of older window entries), and
// aligned with the objective NRPA already optimises (same `material`, novelty
// excluded, since novelty is a gradient tie-break rather than a measure of
// success, and it is unbounded).
//
// WHY THIS INSTRUMENT IS THE RIGHT ONE. If Tenki searches for something other
// than the right target, the line dies in ~20 decisions; the information EXISTS
// (the rollout ends) but the solver had nowhere to put it, since PolicyRollout
// has no transposition table and its only memory is one weight per move code,
// blind to the state. Q^ is that place: at the first decision s = {}, so
// Q^({},a) is the average reward of the lines that played a, AVERAGED OVER ALL
// THE ROLLOUTS, the bad ones included. It is the missing half of the NRPA
// gradient.

// Sample size and mean reward of an estimator.
struct PermStat {
	uint32_t n = 0;
	float q = 0.0f;
};

inline uint32_t PopCount64(uint64_t x) {
#if defined(_MSC_VER) && defined(_M_X64)
	return static_cast<uint32_t>(__popcnt64(x));
#elif defined(__GNUC__) || defined(__clang__)
	return static_cast<uint32_t>(__builtin_popcountll(x));
#else
	uint32_t c = 0;
	while(x) { x &= x - 1; ++c; }
	return c;
#endif
}

// SLIDING WINDOW OF ROLLOUTS, one bitset per move code. PER WORKER: the
// mechanism has no synchronisation at all, like the novelty table and the
// contextual level. The paper takes W = 10 000; we expose the dial
// (--qhat-window) and print the measured memory, because we pay W SIXTEEN
// TIMES (one set of bitsets per worker) where the paper pays it once.
class PermWindow {
public:
	void Init(uint32_t w) {
		size_ = w ? w : 1;
		words_ = (size_ + 63) / 64;
		reward_.assign(size_, 0.0f);
		present_.assign(words_, 0ull);
		slot_.assign(size_, std::vector<uint64_t>());
		bits_.clear();
		root_.clear();
		pushed_ = 0;
	}
	bool Ready() const { return size_ != 0; }
	uint64_t Pushed() const { return pushed_; }
	size_t Codes() const { return bits_.size(); }

	// Pours in a rollout. `moves` must be SORTED and DEDUPLICATED (the multiset of
	// the codes played; MCPS counts one presence per game).
	void Push(const std::vector<uint64_t>& moves, float r) {
		const uint32_t slot = static_cast<uint32_t>(pushed_ % size_);
		const uint64_t bit = 1ull << (slot & 63);
		const size_t word = slot >> 6;
		if(present_[word] & bit) {
			// The evicted rollout gives its bits back, and the root statistic, which
			// is kept INCREMENTALLY, gives its reward back.
			const float old = reward_[slot];
			for(uint64_t c : slot_[slot]) {
				auto it = bits_.find(c);
				if(it == bits_.end())
					continue;
				it->second[word] &= ~bit;
				bool empty = true;
				for(uint64_t v : it->second)
					if(v) { empty = false; break; }
				if(empty)
					bits_.erase(it);
				auto ir = root_.find(c);
				if(ir != root_.end()) {
					if(--ir->second.n == 0)
						root_.erase(ir);
					else
						ir->second.sum -= old;
				}
			}
		}
		present_[word] |= bit;
		reward_[slot] = r;
		slot_[slot] = moves;
		for(uint64_t c : moves) {
			auto& b = bits_[c];
			if(b.empty())
				b.assign(words_, 0ull);
			b[word] |= bit;
			RootStat& rs = root_[c];
			++rs.n;
			rs.sum += r;
		}
		++pushed_;
	}

	// Mask of the PRESENT rollouts containing ALL the codes of `cond`.
	void Mask(const std::vector<uint64_t>& cond, std::vector<uint64_t>& out) const {
		out = present_;
		for(uint64_t c : cond) {
			auto it = bits_.find(c);
			if(it == bits_.end()) {
				out.assign(words_, 0ull);
				return;
			}
			for(size_t i = 0; i < words_; ++i)
				out[i] &= it->second[i];
		}
	}

	// (n^, Q^) of a code under an already-computed mask: the paper's popcount.
	PermStat Stat(uint64_t code, const std::vector<uint64_t>& mask) const {
		PermStat s;
		auto it = bits_.find(code);
		if(it == bits_.end() || mask.size() != words_)
			return s;
		uint32_t n = 0;
		double sum = 0;
		for(size_t i = 0; i < words_; ++i) {
			uint64_t w = it->second[i] & mask[i];
			if(!w)
				continue;
			n += PopCount64(w);
			while(w) {
				// Index of the low bit: popcount of the mask of the bits below it.
				const uint32_t b = PopCount64((w & (~w + 1ull)) - 1ull);
				sum += reward_[(i << 6) + b];
				w &= w - 1ull;
			}
		}
		s.n = n;
		s.q = n ? static_cast<float>(sum / n) : 0.0f;
		return s;
	}

	// (n^, Q^) AT THE ROOT (empty condition), kept INCREMENTALLY on every pour
	// and eviction. The paper FREEZES a node's permutation statistics at rho
	// visits; it explicitly excludes the root, and with reason: it is the only
	// node whose condition is empty, hence the only one whose statistic can be
	// maintained in O(moves of the rollout) rather than by a sweep. It is also
	// the one that carries the whole diagnosis (the first decision).
	PermStat Root(uint64_t code) const {
		PermStat s;
		auto it = root_.find(code);
		if(it == root_.end() || !it->second.n)
			return s;
		s.n = it->second.n;
		s.q = static_cast<float>(it->second.sum / it->second.n);
		return s;
	}

	// Memory actually occupied, printed in the summary: the mechanism's cost is in
	// MEMORY and it is paid per worker (a mechanism whose cost is not measured
	// ends up being tuned blind).
	size_t Bytes() const {
		size_t b = reward_.size() * sizeof(float) +
				   present_.size() * sizeof(uint64_t) +
				   bits_.size() * (words_ * sizeof(uint64_t) + 48) +
				   root_.size() * 48;
		for(const auto& s : slot_)
			b += s.capacity() * sizeof(uint64_t) + 32;
		return b;
	}

private:
	struct RootStat { uint32_t n = 0; double sum = 0; };
	uint32_t size_ = 0;
	size_t words_ = 0;
	uint64_t pushed_ = 0;
	std::vector<float> reward_;      // reward of each cell's rollout
	std::vector<uint64_t> present_;  // filled cells (bitset)
	// Codes of each cell's rollout: without them, evicting a rollout would mean
	// sweeping EVERY bitset. This is the dominant memory item, and it is what
	// sets the default for W.
	std::vector<std::vector<uint64_t>> slot_;
	std::unordered_map<uint64_t, std::vector<uint64_t>> bits_;
	std::unordered_map<uint64_t, RootStat> root_;
};

// A node of the HEAD BANDIT: the first k decisions of a rollout.
//
// The node's key is the MIXED SUM of the moves played, which is commutative, so
// two orders leading to the same multiset share their statistics. That is both
// the project's guiding principle (search over the state GRAPH: A then B and B
// then A converge) and the reading of MCPS, whose permutations say exactly
// that.
struct BanditNode {
	uint32_t visits = 0;
	bool frozen = false;
	// Multiset of the path's moves: Q^'s CONDITION at this node.
	std::vector<uint64_t> cond;
	// FROZEN mask of the rollouts satisfying `cond`, computed once when the node
	// reaches rho visits (the paper's freeze). Empty while the node has not
	// frozen: its descendants then read the nearest frozen ancestor, the root at
	// worst, which is exactly MCPS's propagation.
	std::vector<uint64_t> mask;
	// Cache of (n^, Q^) per code under this mask. The paper computes the
	// statistics of ALL the codes at once on freezing; we compute them on demand
	// and keep them. Same semantics (one value per (node, code), frozen from the
	// first use), with memory proportional to the moves ACTUALLY offered at this
	// node (a few dozen) rather than to the whole vocabulary.
	std::unordered_map<uint64_t, PermStat> perm;
	// Q(s,a): sample size and sum of the rewards of the rollouts that went through
	// THIS node and then played a.
	std::unordered_map<uint64_t, std::pair<uint32_t, double>> q;
};

// One row of the bandit's PROBE (aggregated across workers by the caller): the
// mandatory instrument BEFORE any measurement, because the corpus agreement
// curve CANNOT judge Q^ (it measures the reproduction of a corpus containing
// ONLY good lines, whereas Q^ draws its signal from the FAILURES).
struct BanditProbe {
	uint64_t key = 0;
	uint32_t code = 0;    // card engaged by the move (0 = macro or unknown)
	uint32_t n = 0;       // sample size of Q
	double w = 0;         // sum of Q's rewards
	uint32_t nhat = 0;    // sample size of Q^
	double qhat_sum = 0;  // n^ * Q^ (additive across workers)
};

// SEMANTIC compatibility between the current context and that of an occurrence
// of a macro in the corpus. The POSITIONAL window does not work: rollouts do
// line up by index with the corpus, so the precondition has to be the STATE.
// Target cards placed: EXACT (the combo's axis of progress, the one that
// separates the climb from the finisher); hand: within +/-hand_tol (searches
// make it oscillate, and exactness would over-constrain the way the window
// did).
inline bool OptionCtxCompatible(uint16_t have, uint16_t want, uint32_t hand_tol) {
	if((have >> 4) != (want >> 4))
		return false;
	const uint32_t a = have & 15u, b = want & 15u;
	return (a > b ? a - b : b - a) <= hand_tol;
}

// Effective weight of a move under the two-level policy. `shrink` < 0 turns the
// contextual level off (the function then returns pol[key] exactly).
inline float EffectiveWeight(const NrpaPolicy& pol, const NrpaResidual* res,
							 uint64_t key, uint64_t ctx, float shrink) {
	auto it = pol.find(key);
	const float wg = (it == pol.end()) ? 0.0f : it->second;
	if(!res || shrink < 0.0f)
		return wg;
	auto ic = res->find(CtxKey(key, ctx));
	if(ic == res->end())
		return wg;
	const float s = static_cast<float>(ic->second.n) /
					(static_cast<float>(ic->second.n) + shrink);
	return (1.0f - s) * wg + s * ic->second.w;
}

// ADAPTATION REPLAY of the corpus (arXiv:2401.10431): records a solution line
// as a SEQUENCE OF POLICY DECISIONS, i.e. at each multiple-choice prompt of our
// player, the set of LEGAL plan_keys and the index of the one the line played.
// That is exactly what Adapt() consumes, the standard NRPA gradient.
//
// What this brings over a WEIGHT prior (`--prior`, measured NEUTRAL): the prior
// gave a move the same bonus EVERYWHERE; the adaptation is DISCRIMINATIVE. A
// corpus move does not rise in the absolute, it rises AGAINST the moves that
// opposed it at that precise place, and a corpus move systematically discarded
// elsewhere goes back down. That is the difference between "these moves exist"
// and "at this junction, the solution took this one".
//
// The duel must be at the start, and it is the duel of the line's HEADER (one
// never replays a corpus line on the starting duel: only the semantic
// identities carry over). `repertoire` is the index of the reference's moves:
// it is used to reproduce the sampling's `known` bias, without which Adapt()
// would compute a gradient under a distribution that is not the rollouts'.
// Returns the number of unidentified steps (skipped: we do not know which
// choice the line took).
// `target` serves the CONTEXT of each step (target board cards already placed):
// the same descriptor as the one computed at rollout time.
class RecipeGraph;
// `recipes`: when non-null, the REPLAYED summons of the corpus line feed the
// recipe graph with OBSERVED recipes, i.e. real materials and zones, exception
// routes included (materials from the deck, summons without materials,
// substitutes). That is what breaks the chicken-and-egg of the observational
// graph ("it only learns from itself") without reading a single effect text.
// `mcps_depth`: the path conditioning must be computed IDENTICALLY on both
// sides, otherwise the corpus gradient would land in cells the rollouts never
// visit (the exact fault once found on hint_bias). 0 = semantic descriptor, as
// before.
// Records the FACTS of a state, for landmark learning: the multiset (canonical
// code, normalised zone) of the target player. MZONE and SZONE collapse into
// "field", exactly as in the recipe graph and in the goal criterion. Three
// places that must agree, otherwise a landmark "on the field" would never be
// recognised as satisfied. The DECK and the EXTRA are excluded: a card sleeping
// there is not an achievement, it is a reserve.
void CollectStateFacts(Duel& duel, uint8_t con,
					   std::unordered_map<uint64_t, uint32_t>& out);

// Trace of ONE resolved plan, for landmark learning (see LandmarkGraph, below).
// At NAMESPACE level rather than nested in the class: `LiftPolicyRun` fills it
// and is declared BEFORE it, and a pointer to a type nested in an incomplete
// class does not exist.
//
//   `first[key][k-1]`: when the k-th copy of the fact `key` appeared, in
//                      decision indices first, normalised into a FRACTION of
//                      the line by Normalize();
//   `initial[key]`   : the count in the INITIAL state (a landmark is an
//                      ACHIEVEMENT, not a fact that is already true).
struct LandmarkTrace {
	std::unordered_map<uint64_t, std::vector<float>> first;
	std::unordered_map<uint64_t, uint32_t> initial;
	uint32_t decisions = 0;
	// Raw indices become fractions: two plans of different lengths must be able to
	// average their orders of achievement, otherwise the longest plan crushes all
	// the others.
	void Normalize() {
		const float d = static_cast<float>(decisions > 0 ? decisions : 1);
		for(auto& [k, v] : first)
			for(float& x : v)
				x /= d;
	}
};

size_t LiftPolicyRun(Duel& duel, Arena& arena, const Replay& yrp,
					 int target_player, size_t stop_after, const EnumOptions& eo,
					 const std::unordered_map<uint64_t, size_t>& repertoire,
					 const BoardKey& target, NrpaRun& out,
					 RecipeGraph* recipes = nullptr, uint32_t mcps_depth = 0,
					 LandmarkTrace* landmarks = nullptr);

// Mean (log) probability `pol` gives to the moves CHOSEN by the corpus lines,
// under the same biases as the sampling. It is the instrument that says whether
// the adaptation replay bit: on a virgin policy it equals the mean of
// -log(number of legal choices); if it does not rise after the passes, the
// mechanism is inert and there is no point paying for a run to find out.
// Returns the GEOMETRIC mean of p(move played), i.e. exp of the mean
// log-likelihood. `argmax_frac`, when supplied, receives the FRACTION of steps
// where the corpus move is the one the policy ranks first: it is the only one
// of the two comparable with CorpusCoherence's ceiling. Together they separate
// "the policy is a little wrong everywhere" from "it is right almost
// everywhere and collapses on a few steps". A geometric mean is crushed by a
// handful of p close to zero, and read alone it would suggest failure where
// there is none.
double CorpusAgreement(const NrpaPolicy& pol, const NrpaResidual* res,
					   const std::vector<NrpaRun>& runs, float bias_known,
					   float shrink, double* argmax_frac = nullptr);

// SEARCH COST FORECAST, computed on lines that are ALREADY solved.
//
// The Levin Tree Search guarantee bounds the number of expansions by d/pi(sol),
// where pi is the PRODUCT of the policy's probabilities along the line. That is
// exactly the quantity measured as "mass", so it is what governs the finisher's
// cost, not the ranking.
//
// sqrt-LTS (arXiv:2412.05196) implicitly decomposes the search into q subtasks
// anchored on HINTS ("a hint may be given as soon as a subtask is solved"). Our
// code already produces those hints at every node: the number of target board
// cards placed, which is the high component of PolicyStep::ctx. A search
// decomposed over those q points costs, at best, the SUM of the per-segment
// bounds instead of the global product.
//
// This function computes both, on the corpus lines, before a single line of
// algorithm is written: it returns log10 of the monolithic bound and log10 of
// the decomposed bound. The gap between them IS the gain sqrt-LTS can deliver
// at best; the paper adds a factor tied to the rerooter's uncertainty on top,
// so it is a ceiling, not a promise.
struct CostForecast {
	double log10_mono = 0;    // log10 of d/pi over the whole line
	double log10_decomp = 0;  // log10 of sum_i d_i/pi_i
	double segments = 0;      // mean q (hint points + 1)
	double worst_seg_log10 = 0;   // the most expensive segment, log10 of d_i/pi_i
	size_t lines = 0;
};
CostForecast ForecastSearchCost(const NrpaPolicy& pol, const NrpaResidual* res,
								const std::vector<NrpaRun>& runs,
								float bias_known, float shrink);

// OPTION GAIN FORECAST, computed BEFORE writing the mechanism, as alpha was for
// sqrt-LTS.
//
// The argument is arithmetic and it is the only one in the review that touches
// the EXPONENT: the wall is 0.74^160. An option, a temporally extended action,
// divides the exponent: if "summon Kaleido Chick and send Leo Dancer to the
// graveyard" is ONE action instead of eight decisions, a 160-decision line
// becomes a ~20-decision line.
//
// WHAT THIS FUNCTION DOES. It mines the frequent subsequences of the corpus's
// PLAYED moves (Macro-FF, arXiv:1109.2154; Castellanos-Paez,
// arXiv:1810.09145), then recomputes the Levin bound d/pi as if those
// subsequences were atomic actions, i.e. the selection criterion of Alikhasi &
// Lelis (arXiv:2410.11262), which picks options by MINIMISING the Levin loss.
//
// UNDER A UNIFORM POLICY, and that is deliberate: the paper selects that way,
// and above all it measures what the options bring ON THEIR OWN, without
// confusing their contribution with what the learned policy already knows.
//
// TWO ACKNOWLEDGED CONSERVATISMS, so the figure is a LOWER bound on the gain
// rather than a promise:
//   * every option is assumed offered at EVERY decision (the denominator
//     carries the whole catalogue everywhere), whereas an option is only
//     rarely applicable;
//   * no option is credited with making a line shorter than what the corpus
//     actually played.
struct OptionForecast {
	size_t options = 0;      // size of the selected catalogue
	size_t max_len = 0;      // length of the longest option
	double log10_flat = 0;   // log10 of d/pi, atomic decisions
	double log10_opt = 0;    // log10 of d/pi, options included
	double depth_flat = 0;   // mean depth of a corpus line
	double depth_opt = 0;    // mean depth after substitution
	double covered = 0;      // fraction of decisions absorbed by an option
	size_t lines = 0;
};
OptionForecast ForecastOptionGain(const std::vector<NrpaRun>& runs,
								  size_t max_options, uint32_t min_support,
								  size_t max_len);

// --- OPTION CATALOGUE ---
//
// The only identified lever that attacks the EXPONENT: the forecast puts the
// big catalogue (256 macros, support 2, length <= 8) at 93 % absorption of the
// corpus and 8.5 orders of magnitude on the Levin bound.
//
// A macro is a SEQUENCE OF plan_keys mined from the --adapt corpus. In the
// sampling it is ONE unit: offered alongside the prompt's choices when its
// first key is legal there, weighted by ITS OWN policy weight (its own id in
// the same key space), and once chosen, its following keys are played without
// sampling and without producing a PolicyStep. That is where the line's depth
// (in the sense of the Levin loss) falls from 170 to ~32. If a key is not
// offered by the current prompt, the macro ABORTS and the rollout resumes its
// normal course: the mechanism never removes anything from the space.
struct OptionCatalog {
	std::vector<std::vector<uint64_t>> seqs;
	std::vector<uint64_t> ids;   // policy key of each macro
	// MEAN position (in recorded decisions) of the macro's occurrences in the
	// corpus: the proxy precondition. The macro-operators of the literature
	// carry preconditions; without them, a subsequence mined at decisions 40-47
	// is offered from decision 5 on, where its first key is legal but its
	// continuation is not.
	std::vector<uint32_t> pos;
	// first key -> indices of the macros starting with it (selection order:
	// front() = the best ranked)
	std::unordered_map<uint64_t, std::vector<uint32_t>> by_first;
	// Offer window: the macro is only offered while
	// |current decision - pos| <= window. 0 = no guard.
	uint32_t window = 0;
	// SEMANTIC precondition (the alternative to the window, which does not work):
	// the DISTINCT contexts (PolicyStep::ctx) recorded at the START of each macro's
	// occurrences
	// in the corpus. The macro is only offered when the current context is
	// compatible with one of them (exact target cards placed, hand within
	// +/-ctx_tol). ctx_tol < 0 = guard off.
	std::vector<std::vector<uint16_t>> ctxs;
	int ctx_tol = -1;
	bool CtxOk(size_t m, uint16_t ctx) const {
		if(ctx_tol < 0)
			return true;
		for(uint16_t w : ctxs[m])
			if(OptionCtxCompatible(ctx, w, static_cast<uint32_t>(ctx_tol)))
				return true;
		return false;
	}
	// MODEL Levin loss (log10, mean per line, uniform policy) without then with
	// the catalogue: the selection instrument.
	double model_flat = 0, model_opt = 0;
	size_t Size() const { return seqs.size(); }
};
// GREEDY SELECTION BY LEVIN LOSS (Alikhasi & Lelis, arXiv:2410.11262) rather
// than by raw gain support x (length - 1): every macro added swells the
// denominator of EVERY decision where it can be offered, so the selection stops
// by itself once that cost exceeds the absorption. Without it the catalogue
// floods the softmax.
// Approximations documented in the implementation: candidate pool capped at the
// 1024 best raw candidates, beam of 64 per round, denominator not capped at one
// macro per first key (reality is cheaper than the model), and the paper's
// log10(d) term neglected (~2 against ~87).
OptionCatalog MineOptionCatalog(const std::vector<NrpaRun>& runs,
								size_t max_options, uint32_t min_support,
								size_t max_len, uint32_t window, int ctx_tol);

// --- ONLINE OPTION MINING ---------------------------------------------------
//
// Precedent: Marvin (arXiv:1110.2736) memoises its macros DURING the search and
// uses them in the same solve. Until now our catalogue was mined ONCE, at
// startup, on an EXTERNAL corpus (`--adapt`): a run starting from nothing
// stayed bare to the end, and the bootstrap loop needed several runs. Here the
// loop moves INSIDE the run: the workers pour their best lines into a LIVING
// corpus, a miner periodically re-runs `MineOptionCatalog` over it (same
// selection by Levin loss, arXiv:2410.11262), and the catalogue is swapped at a
// SAFE BOUNDARY.
//
// THREE DESIGN POINTS, each forced by a constraint of the code:
//
//  1. BOUNDARY. The workers read the catalogue as const, and `PolicyRollout`
//     keeps a RAW pointer into `seqs[m]` for the duration of an active macro.
//     So the swap happens between two upper-level iterations (no rollout in
//     flight in that worker), and the old catalogue stays alive as long as a
//     worker holds it: one `shared_ptr<const>` per worker, and never a delete
//     under anyone's feet.
//  2. THE LEARNED WEIGHTS SURVIVE re-mining: a macro's id is a HASH OF ITS
//     CONTENT (see MineOptionCatalog), so a macro found again on the next round
//     recovers exactly its policy weight. Nothing to transfer.
//  3. THE MINER DOES NOT BLOCK. The worker claiming the round copies the corpus
//     under lock, RELEASES, mines, then republishes under lock. Mining runs
//     inside the run's budget: its duration is measured and printed (judging it
//     is the condition of keeping it).
//
// DIVERSITY. Across multiple runs it came from the seeds; in a single run it
// has to come from within. So the living corpus is a bounded set with a PER
// WORKER QUOTA: dedup by line signature, at most `per_worker` lines per worker,
// global ceiling `max_pool` by evicting the worst. Without the quota, the
// sixteen workers, which all restart from the best shared sequence, would fill
// the corpus with the same line.
struct OnlineOptions {
	std::mutex mu;
	struct Entry {
		uint64_t sig = 0;      // hash of the moves played: the line's identity
		double score = 0;
		uint32_t worker = 0;
		NrpaRun run;           // FLAT line (NrpaRun::flat promoted into `steps`)
	};
	std::vector<Entry> pool;
	// Lines from an EXTERNAL corpus (--adapt), when the run has one: they take part
	// in the mining without ever being evicted, and count towards no quota.
	const std::vector<NrpaRun>* seed = nullptr;
	size_t max_pool = 12;
	size_t per_worker = 2;
	size_t min_lines = 2;      // below this, nothing worth mining has any support
	// Current catalogue. Null = the workers roll BARE (the starting state of a run
	// without --adapt: exactly the "zero external corpus" mission).
	std::shared_ptr<const OptionCatalog> cat;
	uint64_t gen = 0;
	// Mining parameters, taken from the --options* flags.
	size_t max_options = 256;
	uint32_t support = 2;
	size_t max_len = 8;
	uint32_t window = 0;
	int ctx_tol = -1;
	// Cadence.
	double period_ms = 90000;
	std::chrono::steady_clock::time_point next{};
	bool mining = false;
	// Instrument: a mechanism whose liveness is not read cannot be judged.
	// `mine_ms_max` is the reading that decides whether it stays online.
	uint32_t rounds = 0;
	double mine_ms_total = 0, mine_ms_max = 0;
	uint64_t offered = 0, kept = 0, dups = 0;
	size_t last_lines = 0, last_size = 0, last_maxlen = 0;
	double last_avglen = 0, last_flat = 0, last_opt = 0;
};

// CEILING of the policy family, measured on the corpus itself.
//
// A policy of this shape is a function of the pair (context, set of legal
// moves): two steps presenting the SAME set of choices in the SAME context are
// indistinguishable to it, whatever the weights hold. If the corpus plays
// different moves there, the gap is IRREDUCIBLE: no pass, no k, no enrichment
// of the weights will close it.
//
// So we group the steps by (context, legal set) and return the fraction of
// steps that play the MAJORITY move of their group: that is exactly what the
// best deterministic policy of this family would reach. Comparing that ceiling
// with the agreement obtained says whether to keep enriching the context, or
// whether the corpus contradicts itself.
// `use_ctx` false ignores the context: the difference between the two ceilings
// quantifies what the descriptor brings, independently of learning.
double CorpusCoherence(const std::vector<NrpaRun>& runs, bool use_ctx,
					   size_t* groups = nullptr);

// One NRPA adaptation step over a sequence (Cazenave's gradient: +alpha on the
// move played, -alpha*p on each of the legal ones). Free rather than a member
// so that the corpus recording and the workers apply EXACTLY the same update;
// an instrument measuring something other than what the run undergoes measures
// nothing. When `res` is non-null the contextual level receives the SAME
// gradient on its own (move, context) cell; the two levels estimate the same
// quantity at different granularities.
//
// The default for `temp` is kept here for calls with a single distribution
// argument, but AdaptCorpus REQUIRES both: calling with seven arguments out of
// eight forces hint_bias to 0 and leaves temp at its default, so the gradient
// would not be computed under the rollouts' distribution.
// `ctx_max`: cap on the number of entries of the contextual level (0 =
// unlimited). Past it, existing cells keep being updated and no new one is
// created, so path conditioning degrades gracefully towards the global level.
// `to_peak`: adapt only the PREFIX that produced the score. The score being a
// MAX over prefixes, the steps after the peak contributed nothing and were
// nevertheless reinforced as much as the others.
void AdaptRun(NrpaPolicy& pol, NrpaResidual* res, const NrpaRun& run,
			  float alpha, float bias_known, float hint_bias, float shrink,
			  float temp = 1.0f, size_t ctx_max = 0, float assign_bias = 0.0f,
			  bool to_peak = false);

// `passes` adaptation passes over each corpus line. `hint_bias` and `temp` are
// REQUIRED, with no default: they were once omitted at the call site (seven
// arguments for eight parameters), so the corpus gradient was computed under a
// distribution different from the one the rollouts sample, precisely on the
// hinted moves. A default here would make the same omission silent again.
void AdaptCorpus(NrpaPolicy& pol, NrpaResidual* res,
				 const std::vector<NrpaRun>& runs, uint32_t passes, float alpha,
				 float bias_known, float hint_bias, float shrink, float temp,
				 size_t ctx_max = 0);

// --- state archive (Go-Explore, arXiv:2004.12919) --------------------------
//
// "First return, then explore": keep, DURING the search, the K best DISTINCT
// states, each with the path leading to it, then restart from each of them. The
// "return" is already paid for here (prefix replay, 0.05 ms arena restore);
// what was missing was the archive: the single-state finisher dug into the ONE
// best state, measured exhausted three times in ~6 states (the space is locked
// there from the summon on).
// Cell = hash of the complete board (codes, positions, materials, counters):
// two lines ending on the same board are conflated and only the cheaper one is
// kept. That is the "variants of no interest" merge.
struct ArchiveEntry {
	uint64_t cell = 0;       // BoardKey.hash of the state
	// Under --resolve: resolutions<<44 | overlap<<36 | ~decisions, i.e. the RIPPED
	// states first (measured: with overlap first, the mute 8/8 states evict every
	// 3-rip state from the archive, and the finisher never has a ripped root to
	// close). Without --resolve: overlap<<40 | ~decisions.
	// In ANYTIME mode (cost optimisation), burned cards come before the short
	// path: resolutions<<48 | overlap<<40 | ~burned<<32 | ~decisions (and
	// overlap<<40 | ~burned<<32 | ~decisions without --resolve).
	uint64_t score = 0;
	uint32_t overlap = 0;    // target board cards placed
	uint32_t resolves = 0;   // required resolutions already done
	uint32_t decisions = 0;  // path length
	uint32_t burned = 0;     // partial cost (graveyard + banished), anytime
	std::vector<std::vector<uint8_t>> path;
};

// --- shared transposition table (lazy SMP) ---------------------------------
//
// Atomic slots with lossy overwrite: one entry = 48-bit tag | 16-bit budget.
// With private tables the LDS workers redo the same work; here a state solved
// by one prunes for all. Losing an entry (slot collision) only costs
// redone work; a false positive would require a 64-bit digest collision.
// Minimum size 1 MB (2^17 slots), otherwise some digest bits would take part in
// neither the tag nor the index.
class SharedTT {
public:
	explicit SharedTT(size_t mb) {
		size_t want = ((mb ? mb : 1) << 20) / sizeof(std::atomic<uint64_t>);
		size_t n = 1;
		while(n * 2 <= want)
			n *= 2;
		slots = std::vector<std::atomic<uint64_t>>(n);
		for(auto& s : slots)
			s.store(0, std::memory_order_relaxed);
		mask = n - 1;
	}
	// true when the state has already been reached with a budget >= budget: prune.
	// Otherwise records it (lossily) and returns false. Same semantics as the
	// private table: the entry is written BEFORE the subtree is explored (a "taken"
	// marker), so a timeout can lose a claimed subtree. That is the acknowledged
	// lossiness of lazy SMP, with no effect on passes that finish.
	// `fresh` (optional): the slot did NOT carry this tag, so the state is seen for
	// the first time. It is the only way to keep a count of distinct states on this
	// path. Without it, `distinct_by_depth` stayed at zero in every multi-worker
	// run and still read like a measurement.
	// Lossy like the rest: a slot collision recounts a state already seen.
	bool CheckAndClaim(uint64_t key, uint32_t budget, bool* fresh = nullptr) {
		std::atomic<uint64_t>& s = slots[key & mask];
		const uint64_t tag = key & ~0xffffull;
		const uint64_t cur = s.load(std::memory_order_relaxed);
		const bool same_tag = (cur & ~0xffffull) == tag;
		if(fresh)
			*fresh = !same_tag;
		if(same_tag && (cur & 0xffffull) >= budget)
			return true;
		s.store(tag | (budget > 0xffffu ? 0xffffu : budget),
				std::memory_order_relaxed);
		return false;
	}

private:
	std::vector<std::atomic<uint64_t>> slots;
	uint64_t mask = 0;
};

// --- dynamic work partitioning between workers ------------------------------
//
// One cell per DISTINCT CLAIM POINT, in open addressing on the key itself.
// Replaces an earlier hashed-index array that had two fatal, silent defects:
// two distinct points could land on the same cell (the second was then
// forbidden to everyone), and the array was sized on the size of the PLAN,
// hence ONE cell in goal-only mode, where the plan is empty. One token for
// sixteen workers: every deviation was suppressed and no counter said so.
//
// Two rules:
//   - the key is stored IN FULL, so a refused cell is refused because the point
//     is already taken, never by collision;
//   - on saturation we fail OPEN (the point is granted). Redone work costs
//     time; a lost point would be a hole in completeness, hence a false proof
//     of absence. The `overflow` counter says so.
class ClaimTable {
public:
	explicit ClaimTable(size_t want) {
		size_t n = 1024;
		while(n < want * 4)
			n *= 2;
		slots = std::vector<std::atomic<uint64_t>>(n);
		for(auto& s : slots)
			s.store(0, std::memory_order_relaxed);
		mask = n - 1;
	}
	// true = this worker takes the point; false = another had already taken it.
	bool Claim(uint64_t key) {
		if(!key)
			key = kZeroSub;
		uint64_t h = key * 0x9E3779B97F4A7C15ull;
		h ^= h >> 29;
		for(size_t i = 0; i < kProbe; ++i) {
			std::atomic<uint64_t>& s = slots[(h + i) & mask];
			uint64_t cur = s.load(std::memory_order_relaxed);
			if(cur == key)
				return false;
			if(cur == 0) {
				uint64_t expected = 0;
				if(s.compare_exchange_strong(expected, key,
											 std::memory_order_relaxed))
					return true;
				if(expected == key)
					return false;   // race lost on the SAME point
			}
		}
		overflow.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	uint64_t Overflow() const {
		return overflow.load(std::memory_order_relaxed);
	}

private:
	// A null key is a legitimate digest; we substitute it to keep 0 as the free
	// cell sentinel. Conflating the two values would require both to exist at the
	// same claim level, with no correctness consequence (at worst a point wrongly
	// refused).
	static constexpr uint64_t kZeroSub = 0x5bf03635e2c1a9d7ull;
	static constexpr size_t kProbe = 16;
	std::vector<std::atomic<uint64_t>> slots;
	std::atomic<uint64_t> overflow{ 0 };
	uint64_t mask = 0;
};

// --- RECIPE GRAPH -----------------------------------------------------------
//
// THE PROBLEM IT SOLVES. Our `h`, the number of missing target board cards, is
// zero over ~90 % of the line: the landscape is FLAT, and a decomposition
// mechanism (sqrt-LTS) has nothing to decompose on it. What is missing is not
// the search algorithm, it is a distance that DECREASES while one builds.
//
// THE SHAPE. The combo is a retrosynthesis under a starting-material
// constraint: target board <-> molecule, deck <-> purchasable building blocks,
// opening hand <-> imposed material, summon <-> reaction, burned piece <-> dead
// end by consumption (DESP, arXiv:2407.06334). One cannot invert a DUEL STATE,
// but one can invert a SUMMON, and no model is even needed: the core TELLS US
// what was consumed.
//
// THE THREE RULES, non-negotiable.
//
// 1. THE NODE IS A REQUIREMENT, NOT A CARD: (effective code, zone). "Leo Dancer
//    IN THE GRAVEYARD", not "the card Leo Dancer". A card that COPIES a name
//    (Kaleido Chick taking Leo Dancer's name) then becomes one more SUPPLIER
//    under the same node, not an exception to handle, and it comes for free,
//    because the observed code is already the EFFECTIVE code.
//
// 2. THE GRAPH NEVER PRUNES, IT WEIGHTS. A product with no known recipe returns
//    the FLOOR distance (1), never infinity. If it acted as an oracle, an
//    unmodelled route would silently delete solutions. At worst, `h` falls back
//    to today's flat `h` and nothing is lost; that is the guarantee that makes
//    the mechanism safe to enable.
//
// 3. THE TRUTH COMES FROM OBSERVATION, not from card text. Every executed
//    summon says which entities were consumed and from which zones. No
//    dictionary to maintain, no network to train.

// KIND of requirement. The OR node of rule 1 is not always nameable by a code:
// half the material lines of an extra deck monster require NO precise card
// (measured on benchmark A: 8 cards out of 10).
//
//   "Lunalight Leo Dancer" + 3 "Lunalight" monsters   <- 1 named + 3 archetype
//   2 Level 4 monsters                                <- 2 by level, 0 named
//
// A CARDINAL requirement ("three Lunalight monsters") also carries the gradient
// the flat `h` lacks: it decreases by one for every Lunalight placed, i.e.
// BEFORE any target card is on the field.
enum ReqKind : uint8_t {
	kReqCard = 0,      // `code` is a canonical card code
	kReqSetcode = 1,   // `code` is an archetype setcode (0xdf = Lunalight)
	kReqLevel = 2,     // `code` is a LEVEL (2 Level 4 monsters -> code 4)
};

struct Requirement {
	uint32_t code = 0;    // effective CANONICAL code, or setcode, or level
	uint8_t zone = 0;     // NORMALISED zone (see NormalizeZone)
	uint8_t kind = kReqCard;
	// Copies required. ALWAYS 1 for kReqCard: "2 \"Name\"" is posted as two
	// requirements of one copy, because a named card resolves by presence and by
	// recursion over its recipe, not by a counter. That is what lets Distance's
	// memo leave this field out.
	uint8_t count = 1;
	bool operator==(const Requirement& o) const {
		return code == o.code && zone == o.zone && kind == o.kind &&
			   count == o.count;
	}
};

// JOKER zone: "anywhere a material can be TAKEN from". Card text names a
// material without saying where it comes from, and that is correct: a Fusion
// takes its materials from the field, from the hand, sometimes from the
// graveyard or the banished zone. A requirement seeded from text therefore
// carries 0, and observation, which always produces a concrete zone, then
// refines what the text left open (rule 3).
//
// BUT "anywhere" INCLUDES NEITHER THE DECK NOR THE EXTRA DECK, and that is the
// difference between a mechanism that bites and an inert one. A card sleeping
// in the extra deck is not an available material: it has to be summoned first,
// and that is EXACTLY the step the graph must count. Counting the extra made
// "Lunalight Leo Dancer present" true from the first node (Leo is there in two
// copies), so the distance to Liger Dancer fell back to its floor of 1, i.e. to
// the flat `h`, in the one case where the seeding has something to say. The
// mechanism was LIVE and had no effect.
constexpr uint8_t kZoneAny = 0;

// The zones where a material is AVAILABLE, as opposed to those where a card is
// only in reserve. Used by the joker above.
inline bool ZoneIsPlayable(uint8_t normalized_zone) {
	return normalized_zone == 0x0c ||   // field (MZONE | SZONE)
		   normalized_zone == 0x02 ||   // hand
		   normalized_zone == 0x10 ||   // graveyard
		   normalized_zone == 0x20;     // banished
}

// ocgcore's TYPE_MONSTER. A cardinal requirement always bears on MONSTERS
// ("3 \"Lunalight\" monsters"): counting the archetype's spells and traps would
// satisfy it without any material being available.
constexpr uint32_t kRecipeTypeMonster = 0x1;

// Setcode matching, as the core defines it. A setcode fits in 16 bits: the low
// 12 bits are the archetype, the high 4 bits a sub-archetype. "Lunalight"
// (0x9d) must match "Lunalight Dancer" (0x109d) but not the other way round,
// hence the asymmetry of the high mask.
inline bool SetcodeMatches(uint16_t card_setcode, uint16_t wanted) {
	if((card_setcode & 0x0fffu) != (wanted & 0x0fffu))
		return false;
	return (card_setcode & wanted & 0xf000u) == (wanted & 0xf000u);
}

// Zones reduced to six buckets. MZONE and SZONE are conflated into "field": the
// distinction does not change what a recipe can consume, and keeping it would
// cost one more zone query per evaluation. Any unknown zone falls into "field"
// rather than creating a phantom bucket.
//
// The normalisation must be applied on BOTH sides, when observing the material
// and when testing presence, otherwise a requirement would never be satisfied
// and the distance would be wrong in the dangerous direction (overestimated
// everywhere, hence flat again).
inline uint8_t NormalizeZone(uint8_t loc) {
	if(loc & 0x02) return 0x02;   // HAND
	if(loc & 0x10) return 0x10;   // GRAVE
	if(loc & 0x20) return 0x20;   // REMOVED
	if(loc & 0x40) return 0x40;   // EXTRA
	if(loc & 0x01) return 0x01;   // DECK
	return 0x0c;                  // ONFIELD (MZONE | SZONE)
}

class RecipeGraph {
public:
	// SHARED BETWEEN WORKERS. Recipes are observed facts: merging them can only
	// enrich, never contradict, and that is what makes a single graph legitimate.
	// But sixteen workers write into it, so access is locked. The lock is taken
	// ONCE at the top: `Distance` is recursive, and a non-recursive mutex taken at
	// every level would deadlock against itself.
	//
	// Cost: `Observe` is rare (one summon), `Distance` is called once per EXPANDED
	// node of the finisher, never in the rollouts. So the rollouts' hot path never
	// takes this lock.
	// `primed` = recipe SEEDED FROM TEXT, as opposed to a summon actually observed.
	// The distinction is not decorative: the seeding does not model everything the
	// text requires (no type, no attribute, no race, no "different names"), so its
	// recipes stay SYSTEMATICALLY cheaper than the real ones. A naive `min` would
	// let them win every time and observation could NEVER correct them, which turns
	// rule 3 ("text only seeds it; the truth comes from observation") exactly
	// upside down.
	void Observe(uint32_t product, const std::vector<Requirement>& mats,
				 bool primed = false) {
		if(!product)
			return;
		for(const Requirement& m : mats)
			if(m.kind != kReqCard) {
				has_cardinal.store(true, std::memory_order_relaxed);
				break;
			}
		// CANONICAL order before comparison: the same summon seen with its materials in
		// two orders is THE SAME recipe; without the sort it occupied two of the eight
		// slots per product.
		std::vector<Requirement> canon = mats;
		std::sort(canon.begin(), canon.end(),
				  [](const Requirement& a, const Requirement& b) {
					  if(a.kind != b.kind) return a.kind < b.kind;
					  if(a.code != b.code) return a.code < b.code;
					  if(a.zone != b.zone) return a.zone < b.zone;
					  return a.count < b.count;
				  });
		std::lock_guard<std::mutex> lk(mx);
		auto& list = recipes[product];
		for(Recipe& r : list) {
			if(r.materials == canon) {
				++r.seen;
				// A recipe first seeded and then OBSERVED becomes an observation:
				// the text was telling the truth.
				r.primed = r.primed && primed;
				return;
			}
		}
		if(list.size() < kMaxPerProduct)
			list.push_back({ std::move(canon), 1, primed });
	}

	// Sum of the distances to several products, under one presence state.
	//
	// ONE lock for the whole call, and ONE memo: without it the recursion is
	// exponential, up to `recipes x materials` per level, i.e. ~40^6 in the worst
	// case, which would HANG the finisher rather than slow it down. The memo is
	// valid for the duration of the call because `present` is constant there.
	//
	// Never infinite (rule 2). `budget` bounds the depth: recipes form a cyclic
	// graph (A consumes B here, B consumes A elsewhere) and nothing guarantees it
	// is acyclic.
	//
	// INTRA-RECIPE CONSUMPTION. `avail` is no longer a plain counter: in the TOP
	// frame (the target product's recipe and its direct materials, claim_depth 2
	// then 1), every requirement served CLAIMS its entities, so one body can no
	// longer be both the named material "Leo Dancer" and one of the "3 Lunalight
	// monsters" of the SAME summon. Claims restart at zero between recipes (the min
	// compares independent summons) and between products. Below that (claim_depth
	// 0), the old shared counting and the memo apply: an inter-summon consumption
	// would badly model bodies recycled through the graveyard (possible
	// overestimation, the forbidden direction), and a memo under a claim state
	// would make the distance depend on the visit order.
	template<typename Avail>
	uint32_t DistanceAll(const std::vector<uint32_t>& codes, uint8_t zone,
						 Avail& avail, uint32_t budget = 6) const {
		std::lock_guard<std::mutex> lk(mx);
		std::unordered_map<uint64_t, uint32_t> memo;
		uint32_t total = 0;
		for(uint32_t c : codes)
			total += DistanceLocked(Requirement{ c, zone, kReqCard, 1 }, avail,
									budget, 2, memo);
		return total;
	}

	size_t Products() const {
		std::lock_guard<std::mutex> lk(mx);
		return recipes.size();
	}
	// Does the graph know a recipe for this product? Without that question, a
	// distance of 1 is AMBIGUOUS: it means both "no known recipe, return the floor"
	// (rule 2) and "one summon is enough, every material is there", two OPPOSITE
	// readings. So any probe reading a distance of 1 must disambiguate here.
	bool Knows(uint32_t product) const {
		std::lock_guard<std::mutex> lk(mx);
		auto it = recipes.find(product);
		return it != recipes.end() && !it->second.empty();
	}
	// Does the graph carry at least one CARDINAL requirement? Without it, recording
	// the level and archetypes of every present entity would be a cost paid for
	// nothing at every expanded node. The flag is set at seeding time and never
	// cleared: an observation does not create one.
	bool HasCardinal() const { return has_cardinal.load(std::memory_order_relaxed); }
	// --- CHEAP PATH FOR THE ROLLOUTS ----------------------------------------
	//
	// The graph learns DURING the run, so it carries a mutex, so it is forbidden on
	// the hot path: sixteen workers taking it at every decision serialise the
	// search. The answer is not to drop the lock (the writes are real) but to take
	// it only RARELY: every worker copies the graph locally every N rollouts and
	// evaluates on its copy, whose mutex is uncontended (~20 ns). Same trade-off as
	// the online option catalogue: a snapshot swapped at a safe boundary.
	void CopyInto(RecipeGraph& out) const {
		std::lock_guard<std::mutex> lk(mx);
		out.recipes = recipes;
		out.has_cardinal.store(has_cardinal.load(std::memory_order_relaxed),
							   std::memory_order_relaxed);
	}
	// Normalised zones a requirement mentions, as a bit mask; the values of
	// NormalizeZone are already bitwise disjoint. This is what makes the presence
	// scan proportional to what the graph ASKS FOR rather than to the number of
	// zones that exist: `RecipeDistance` queries all five zones, DECK and EXTRA
	// included (~55 entities, half the cost), although no seeded requirement names
	// them (kZoneAny excludes the reserve, see ZoneIsPlayable).
	uint8_t ZoneMask() const {
		std::lock_guard<std::mutex> lk(mx);
		uint8_t m = 0;
		for(const auto& [product, list] : recipes)
			for(const Recipe& r : list)
				for(const Requirement& q : r.materials)
					m |= q.zone == kZoneAny
							 ? static_cast<uint8_t>(0x02 | 0x10 | 0x20 | 0x0c)
							 : q.zone;
		return m;
	}
	// AND/OR EXPANSION OF THE GOAL, flattened (Retro*, arXiv:2006.15820: a summon
	// is an AND node and its materials are its children).
	//
	// One traversal under lock for two consumers: the list of USEFUL codes
	// (resolved assignment) and the ORDERED list of subproducts to build (backward
	// serialisation). Doing them separately would take the lock twice and diverge
	// at the first fix.
	//
	// `out_reqs`: every requirement met, named AND cardinal.
	// `out_products`: the codes that are themselves known products, in POSTFIX
	//   ORDER (a brick BEFORE what it serves) and deduplicated. That is the
	//   decomposition: solving it left to right is building the goal backwards.
	void Expand(const std::vector<uint32_t>& roots, uint32_t budget,
				std::vector<Requirement>& out_reqs,
				std::vector<uint32_t>& out_products) const {
		std::lock_guard<std::mutex> lk(mx);
		out_reqs.clear();
		out_products.clear();
		std::vector<uint32_t> stack;   // cycle detection: the graph has some
		for(uint32_t c : roots)
			ExpandLocked(c, budget, stack, out_reqs, out_products);
	}
	size_t Size() const {
		std::lock_guard<std::mutex> lk(mx);
		size_t n = 0;
		for(const auto& [k, v] : recipes)
			n += v.size();
		return n;
	}

private:
	struct Recipe {
		std::vector<Requirement> materials;
		uint32_t seen = 0;
		bool primed = false;   // from TEXT, not from an observed summon
	};
	// A card rarely has more than a few distinct routes; the cap keeps observation
	// noise (same materials in a different order) from swelling the list without
	// teaching anything.
	static constexpr size_t kMaxPerProduct = 8;

	template<typename Avail>
	uint32_t DistanceLocked(const Requirement& req, Avail& avail,
							uint32_t budget, uint32_t claim_depth,
							std::unordered_map<uint64_t, uint32_t>& memo) const {
		// CARDINAL REQUIREMENT: we do not recurse. No product is named;
		// "three Lunalight monsters" designates no card to build, only a COUNT
		// to reach. Every missing copy costs one unit, which underestimates
		// (placing a monster costs at least one action): the safe direction of
		// rule 2, and the gradient is there, since it decreases with every copy
		// placed, even when no target card reaches the field.
		if(req.kind != kReqCard) {
			const uint32_t have = claim_depth ? avail.CountAndClaim(req)
											  : avail.Count(req);
			return have >= req.count ? 0u : req.count - have;
		}
		// Named: in the claiming frame, serving a requirement CONSUMES its copy,
		// so "2 \"Name\"" stops being satisfied by a single copy.
		if(claim_depth ? avail.Claim(req) : avail.Count(req) != 0)
			return 0;
		if(!budget)
			return 1;   // floor: we never declare anything unreachable
		// The key carries the budget: the value depends on it, and conflating
		// two budgets would make the distance depend on the visit order. It also
		// carries the kind: two requirements with the same `code` and different
		// kinds (a setcode 4 and a card with code 4 are unrelated objects) would
		// otherwise be conflated in the memo.
		// Shift by 16, not by 12: `zone` goes up to 0x40, so `zone << 4` occupies
		// bits 4 to 14 and encroached on `code << 12`, i.e. a false cache hit,
		// i.e. a WRONG and silent distance.
		const uint64_t key =
			(static_cast<uint64_t>(req.code) << 24) |
			(static_cast<uint64_t>(req.kind) << 20) |
			(static_cast<uint64_t>(req.zone) << 4) | budget;
		// The memo only serves OUTSIDE claiming: a value computed under a given
		// claim state is only valid for that state.
		if(!claim_depth) {
			auto mit = memo.find(key);
			if(mit != memo.end())
				return mit->second;
		}
		auto it = recipes.find(req.code);
		if(it == recipes.end() || it->second.empty()) {
			if(!claim_depth)
				memo.emplace(key, 1u);
			return 1;   // unknown recipe -> exactly the flat `h` from before
		}
		// OBSERVATION BEATS TEXT (rule 3). As soon as a real summon of this
		// product has been seen, the seeded recipes are ignored: they are
		// incomplete by construction, hence too cheap, and keeping them in the
		// `min` would mean preferring an estimate to a fact.
		bool has_observed = false;
		for(const Recipe& r : it->second)
			if(!r.primed) { has_observed = true; break; }
		uint32_t best = 0xffffffffu;
		for(const Recipe& r : it->second) {
			if(has_observed && r.primed)
				continue;
			// Every recipe is ONE independent summon: in the top frame,
			// claims restart at zero between two recipes.
			if(claim_depth == 2)
				avail.ResetClaims();
			uint32_t cost = 1;   // the summon itself
			for(const Requirement& m : r.materials)
				cost += DistanceLocked(m, avail, budget - 1,
									   claim_depth ? claim_depth - 1 : 0, memo);
			best = (std::min)(best, cost);
		}
		const uint32_t out = best == 0xffffffffu ? 1u : best;
		if(!claim_depth)
			memo.emplace(key, out);
		return out;
	}

	// Recursive expansion with the lock already held. SEEDED recipes are ignored as
	// soon as a real summon of the product has been seen, the same rule 3 as
	// DistanceLocked, otherwise the decomposition would state the text where
	// observation says otherwise.
	void ExpandLocked(uint32_t code, uint32_t budget,
					  std::vector<uint32_t>& stack,
					  std::vector<Requirement>& out_reqs,
					  std::vector<uint32_t>& out_products) const {
		if(!budget ||
		   std::find(stack.begin(), stack.end(), code) != stack.end())
			return;
		auto it = recipes.find(code);
		if(it == recipes.end() || it->second.empty())
			return;
		bool has_observed = false;
		for(const Recipe& r : it->second)
			if(!r.primed) { has_observed = true; break; }
		stack.push_back(code);
		for(const Recipe& r : it->second) {
			if(has_observed && r.primed)
				continue;
			for(const Requirement& q : r.materials) {
				if(std::find(out_reqs.begin(), out_reqs.end(), q) ==
				   out_reqs.end())
					out_reqs.push_back(q);
				if(q.kind == kReqCard)
					ExpandLocked(q.code, budget - 1, stack, out_reqs,
								 out_products);
			}
		}
		stack.pop_back();
		// POSTFIX: the product comes in AFTER its materials, so the list's order is a
		// BUILD order. A root already in the list (shared by two recipes) keeps its
		// first position, the deepest one, which respects all of its dependencies.
		if(std::find(out_products.begin(), out_products.end(), code) ==
		   out_products.end())
			out_products.push_back(code);
	}

	mutable std::mutex mx;
	std::unordered_map<uint32_t, std::vector<Recipe>> recipes;
	// Outside the mutex: read at every expanded node, written once at seeding.
	std::atomic<bool> has_cardinal{ false };
};

// --- LEARNED LANDMARK GRAPH -------------------------------------------------
//
// THE PROBLEM IT SOLVES. Reaching a subgoal consumes what the next one needs,
// and NOTHING in the search sees it coming: the flat `h` counts the missing
// target cards, and says neither what must be held BEFORE, nor HOW MANY times.
// So a line preparing TWO Liger Dancer must look WORSE for a long time than a
// line placing one right away, and the NRPA adaptation drifts towards the
// latter.
//
// THE PAPER. Hanou, Dumancic & de Weerdt, arXiv:2508.21564: GENERALISED
// landmarks learned from a set of SOLVED instances, where classical extraction
// fails; STATE FUNCTIONS capturing REPETITION; a directed graph with LOOPS for
// repetitive subplans.
//
// THE GAP WITH THE PAPER, stated up front, because an unstated gap is a lost
// verdict. The paper is PDDL: its landmarks are PREDICATES extracted from an
// action model. We have none, since our actions are Lua scripts. So our
// landmarks are extracted from TRACES: every solved plan is replayed
// (LiftPolicyRun) and at each decision we record the MULTISET of facts
// (canonical code, normalised zone). What we lose is the guarantee of logical
// necessity; what we keep is the only thing `h` needs, an order and counts
// learned from plans that DID work.
//
// THE SHAPE OF A LANDMARK: (code, zone, k), i.e. "k copies of this code in this
// zone". The COUNT is what generalises, and the chain
// (code,zone,1) -> (code,zone,2) -> (code,zone,3) IS the paper's repetition
// loop. "Leo Dancer in the graveyard, COUNT" is exactly the object needed.
//
// THE THREE RULES, inherited from the recipe graph and for the same reasons.
//
// 1. A LANDMARK IS AN ACHIEVEMENT, NOT A FACT. What is already true in the
//    initial state is not to be reached: only a count STRICTLY GREATER than the
//    initial one is kept. Without that filter, "3 Fire Formation - Tenki in
//    hand" would be benchmark A's first landmark, satisfied at decision 0, and
//    `h` would be flat again.
//
// 2. THE GRAPH NEVER PRUNES, IT WEIGHTS. An unreached landmark adds 1 to `h`;
//    no state is declared dead, no branch cut. At worst `h` falls back to
//    today's flat `h`, and enabling it cannot make a goal unreachable.
//
// 3. THE INTERSECTION, NOT THE UNION. A landmark must appear in EVERY plan of
//    the corpus; that is its definition (a fact one MUST reach). A fact present
//    in a single line is a contingency of that line, and admitting it would
//    make `h` a distance to ONE particular plan, i.e. a disguised repertoire.
//    With a single plan in the corpus, the intersection is that plan: the graph
//    SAYS so rather than implying a generalisation.
struct Landmark {
	uint32_t key_index = 0;   // position in LandmarkGraph::Keys()
	uint32_t code = 0;
	uint8_t zone = 0;
	uint32_t count = 1;       // k: the k-th copy
	// Order: mean, over the plans, of the fraction of the line at which the k-th
	// copy appeared. This is the graph's axis of progress.
	float order = 0.0f;
};

class LandmarkGraph {
public:
	using PlanTrace = LandmarkTrace;
	static uint64_t KeyOf(uint32_t code, uint8_t zone) {
		return (static_cast<uint64_t>(code) << 8) | zone;
	}
	static uint32_t CodeOf(uint64_t key) {
		return static_cast<uint32_t>(key >> 8);
	}
	static uint8_t ZoneOf(uint64_t key) {
		return static_cast<uint8_t>(key & 0xff);
	}

	void AddPlan(PlanTrace&& t) { plans.push_back(std::move(t)); }
	size_t Plans() const { return plans.size(); }

	// INTERSECTION over all the plans, then mean order. `Build` is called once,
	// outside any worker.
	void Build() {
		items.clear();
		keys.clear();
		zone_mask = 0;
		if(plans.empty())
			return;
		// Candidates: the facts of the FIRST plan. A landmark must be in all of them,
		// so starting from one is enough and avoids sweeping the union.
		for(const auto& [key, lv] : plans[0].first) {
			const uint32_t init0 = Initial(plans[0], key);
			// Highest KEEPABLE level: the min over the plans of the count reached,
			// and it must exceed the initial count of EVERY plan (rule 1).
			uint32_t kmax = static_cast<uint32_t>(lv.size());
			uint32_t init_max = init0;
			double order_sum[64] = {};
			bool all = true;
			for(const PlanTrace& p : plans) {
				auto it = p.first.find(key);
				if(it == p.first.end()) { all = false; break; }
				kmax = (std::min)(kmax, static_cast<uint32_t>(it->second.size()));
				init_max = (std::max)(init_max, Initial(p, key));
			}
			if(!all || kmax <= init_max)
				continue;
			if(kmax > 63)
				kmax = 63;
			for(const PlanTrace& p : plans) {
				const auto& lvp = p.first.find(key)->second;
				for(uint32_t k = init_max; k < kmax; ++k)
					order_sum[k] += lvp[k];
			}
			for(uint32_t k = init_max; k < kmax; ++k) {
				Landmark lm;
				lm.code = CodeOf(key);
				lm.zone = ZoneOf(key);
				lm.count = k + 1;
				lm.order = static_cast<float>(order_sum[k] /
											  static_cast<double>(plans.size()));
				items.push_back(lm);
			}
		}
		// Order of progress: it IS the graph. Sorting by mean order is enough to
		// carry the information `h` needs ("how much is left to achieve"), and the
		// edges only serve to print it readably.
		std::sort(items.begin(), items.end(),
				  [](const Landmark& a, const Landmark& b) {
					  if(a.order != b.order) return a.order < b.order;
					  if(a.code != b.code) return a.code < b.code;
					  if(a.zone != b.zone) return a.zone < b.zone;
					  return a.count < b.count;
				  });
		// Index of the DISTINCT keys: the state counter is only filled for them.
		// That is what makes the evaluation affordable inside the ROLLOUTS: we do
		// not record the state, we record thirty cells.
		for(const Landmark& lm : items) {
			const uint64_t k = KeyOf(lm.code, lm.zone);
			if(std::find(keys.begin(), keys.end(), k) == keys.end())
				keys.push_back(k);
		}
		std::sort(keys.begin(), keys.end());
		for(Landmark& lm : items) {
			const uint64_t k = KeyOf(lm.code, lm.zone);
			lm.key_index = static_cast<uint32_t>(
				std::lower_bound(keys.begin(), keys.end(), k) - keys.begin());
			zone_mask |= ZoneBit(lm.zone);
		}
	}

	const std::vector<Landmark>& Items() const { return items; }
	const std::vector<uint64_t>& Keys() const { return keys; }
	size_t KeyCount() const { return keys.size(); }
	// Which normalised zones the counter has to record. The field comes for free
	// from the board already computed; every other zone costs a query to the core,
	// and is only paid for when a landmark lives there.
	uint32_t ZoneMask() const { return zone_mask; }
	bool Empty() const { return items.empty(); }
	// The 0x80 bit marks an OPPONENT zone (handrip: see CollectStateFacts). So each
	// zone has two bits, and the mask says exactly which queries the hot path has
	// to pay for.
	static uint32_t ZoneBit(uint8_t z) {
		const uint32_t side = (z & 0x80) ? 5u : 0u;
		switch(z & 0x7f) {
		case 0x02: return 1u << (0 + side);   // hand
		case 0x10: return 1u << (1 + side);   // graveyard
		case 0x20: return 1u << (2 + side);   // banished
		case 0x0c: return 1u << (3 + side);   // field
		default:   return 1u << (4 + side);
		}
	}
	// Position of a key in `keys`, or SIZE_MAX. Binary search over a table of a few
	// dozen entries.
	size_t IndexOf(uint64_t key) const {
		auto it = std::lower_bound(keys.begin(), keys.end(), key);
		if(it == keys.end() || *it != key)
			return SIZE_MAX;
		return static_cast<size_t>(it - keys.begin());
	}
	// `h`: landmark achievements still MISSING. `have` is parallel to `keys`. Rule
	// 2: this value cuts nothing, it weights.
	uint32_t Remaining(const std::vector<uint32_t>& have) const {
		uint32_t n = 0;
		for(const Landmark& lm : items)
			if(lm.key_index >= have.size() || have[lm.key_index] < lm.count)
				++n;
		return n;
	}

private:
	static uint32_t Initial(const PlanTrace& p, uint64_t key) {
		auto it = p.initial.find(key);
		return it == p.initial.end() ? 0u : it->second;
	}
	std::vector<PlanTrace> plans;
	std::vector<Landmark> items;
	std::vector<uint64_t> keys;
	uint32_t zone_mask = 0;
};

struct SearchConfig {
	int target_player = 0;
	uint32_t max_decisions = 24;      // depth, in decisions
	uint32_t max_actions = 0;         // 0 = no bound (otherwise A_ref)
	double time_limit_ms = 30000;
	uint64_t max_nodes = 2000000;
	// BUDGET IN ROLLOUTS: THE CONDITION FOR DETERMINISM.
	//
	// WALL TIME as the unit of the budget is the cause of the non-determinism, and
	// not exchanges between workers: two `--threads 1` runs on the same
	// seed, hence WITHOUT any exchange, do 41 232 and 42 179 rollouts. They do not
	// stop at the same point of the NRPA trajectory, so they do not return the same
	// result. The number of workers has nothing to do with it: it is the UNIT of
	// the budget.
	//
	// At `--threads 1` and with this ceiling, two executions do exactly the same
	// work. That is the only mode in which a fine comparison means anything; it is
	// NOT the production mode (measured cost of a single worker: /5.1 to /5.9).
	// 0 = unlimited, i.e. byte-for-byte the previous behaviour.
	uint64_t max_rollouts = 0;
	EnumOptions enumeration;
	bool collect_solutions = true;
	size_t max_solutions = 64;
	// COUNT THE DISTINCT BOARDS, not just the states.
	//
	// The operator's question, and it is a fair one: "99 386 states at depth 20
	// looks absurdly large; how many different BOARDS can one make, position and
	// slot being irrelevant?" The solver counted STATES (`StateDigest`: zones +
	// prompt payload, hence the history of the effects already used) and NEVER
	// counted boards. The ratio of the two says what the fineness of the
	// transposition key costs, and whether it explores the same board a hundred
	// times.
	//
	// Three granularities, from finest to coarsest, all already carried by
	// BoardKey:
	//   `entries`: (zone, code, face, materials, counters), with ATK/DEF position
	//              and column ALREADY ignored;
	//   `loose`  : (zone, code, face), neither materials nor counters;
	//   `codes`  : the codes alone, all zones conflated.
	// Cost: one zone query per expanded node. Reserved for --growth, which is a
	// DIAGNOSTIC mode.
	bool count_boards = false;
	// CANONICALISATION OF THE COLUMNS IN THE TRANSPOSITION KEY. The field is SORTED
	// before being hashed: two states differing only by the column of the cards
	// become the same node. Attribution measurement: x33.7 fewer distinct values at
	// the stable points, by far the largest contributor.
	//
	// DOES NOT PRUNE THE SPACE: no branch is removed from the enumeration, it is
	// the TABLE that merges more. But merging can cut: if two conflated states are
	// not equivalent (LINK arrows), a solution disappears silently. Hence the
	// opt-in and `main`'s warning.
	// Safety net: the final verification replays each candidate from scratch.
	// ELISION OF FORCED MOVES IN THE EXHAUSTIVE SEARCH.
	//
	// WHAT THE ATTRIBUTION POINTS AT. The transposition key is worth ~200 times the
	// number of boards, but refining the key only gives x1.67: most nodes are not
	// decision points, they are INTERMEDIATE INSTANTS (opponent chain windows,
	// single-candidate selections) where there is NOTHING TO DECIDE. The figure was
	// available long before it was used: on the reference line, 141 of the 284
	// prompts are FORCED, and "depth after elision: 143 instead of 284".
	//
	// WHAT THE FLAG DOES. A prompt offering only ONE legal answer is played inline:
	// it costs neither depth, nor a table entry, nor an arena snapshot (no sibling
	// to restore). That is exactly what the finisher already does (FORCED moves are
	// played inline and cost neither depth nor probability); the exhaustive search
	// and the table did not.
	//
	// IT IS NOT PRUNING: no branch is removed, since a prompt with a single answer
	// has no alternative by definition. The only effect is that `max_decisions`
	// counts REAL DECISIONS rather than prompts, so depths only compare at equal
	// TIME budget.
	bool elide_forced = false;

	// Work partitioning between workers. The subtree opened by the FIRST deviation
	// is independent of all the others, which allows sharing with no
	// synchronisation at all during the exploration.
	//
	// The assignment is DYNAMIC: a static split would be badly unbalanced, since
	// deviating early opens a huge subtree and deviating late a tiny one. Every
	// worker walks the reference spine and claims the points still free; the fast
	// workers take more of them.
	// Deviation level at which the claim is made. Claiming the FIRST deviation
	// would be unbalanced: the work concentrates in a few early subtrees, and the
	// workers that catch none finish immediately. By claiming at the second, every
	// worker enters the big subtrees and shares the work there.
	uint32_t claim_level = 1;
	// The claim key is the REFERENCE INDEX in repair (a genuine partition of the
	// line: one point, one token) and the STATE DIGEST in transplantation, where no
	// linear index exists; in goal-only mode there is not even a line. See
	// ClaimTable.
	ClaimTable* claims = nullptr;

	// Transplantation: preference order between two repertoire moves. Used only to
	// visit first the ones the reference played early.
	uint32_t plan_window = 32;
	// Traces the descent along the plan: at each prompt, what was offered and
	// whether the plan was found there. Used to see WHERE a deck falls off.
	bool trace = false;

	// --- novelty pruning ---
	// 0 = disabled. Otherwise: number of consecutive decisions with no new atom
	// after which the branch is cut. Chain resolutions go through mute states, so
	// cutting at the first silence would kill the reference line. The bound is
	// MEASURED (width report), not guessed.
	uint32_t novelty_patience = 0;
	// Partition the atoms by number of subgoals reached: the table reopens at every
	// target card placed (serialisation of the goal).
	bool novelty_serialize = true;
	// SERIALISATION BY THE MATERIAL BALANCE SUBGOALS.
	//
	// THE ARITHMETIC THAT FORCES IT, and it is the only route the measurements
	// leave open: the real line carries ~110 real decisions of geometric arity 5.9.
	// In ONE block that is `5.9^110 ~ 10^85`, impossible at any budget. In 14
	// blocks of 8 it is `14 x 5.9^8 ~ 2x10^7`, reachable. The difference is neither
	// the policy, nor the budget, nor the quotient: it is SERIALISATION.
	//
	// The current criterion (`CommonCodes`: TARGET cards placed) only moves at the
	// very end. This one counts the places of the vector `x*` already served, hence
	// the INTERMEDIATE steps, the ones that exist from the first brick on.
	struct SerialReq {
		uint32_t code = 0;    // 0 = archetype requirement
		uint64_t arch = 0;
		// 1 = AVAILABLE (field+hand+graveyard+banished), 2 = FIELD, 3 = GRAVEYARD,
		// 4 = BANISHED (CONSUMPTION rungs of x*: Wolf/Masquerade summon by BANISHING
		// the materials, and during assembly that is the count that climbs),
		// 5 = RESERVE DEPARTURES (after naming the deserts): one unit per card that
		// left deck+extra since the search root. The intermediate summons, the extra
		// deck VEHICLES that x* does not fire because the LP fuses "directly", all
		// consume the reserve; it is the turn's IRREVERSIBLE resource, and the only
		// quantity that climbs STEADILY during the +46/+47 deserts. `code` and `arch`
		// are ignored for this zone.
		uint8_t zone = 0;
		uint32_t count = 1;
	};
	std::vector<SerialReq> serial_reqs;
	// (SerialProgress is declared at the end of the file: it also serves the
	// finisher root probe in main.cpp.)
	// RETURN TO THE RUNG: the half of SIW_R that the ROLLOUTS were missing. The
	// ladder archive existed and only the FINISHER took its roots there: the
	// sampling phase (99 % of the budget) restarted from the root at every rollout,
	// and the sp_final histogram shows it (mass on the first units, exponential
	// tail). The closed form says the arithmetic `Sigma b^(l_i)` only exists if
	// each block is searched FROM the previous rung. `reenter` is the probability
	// that an NRPA rollout restarts from an archive cell (uniform among the rungs
	// kept; Go-Explore: high rungs can be dead ends, so the whole budget is not
	// concentrated on them) instead of from the root.
	// Only acts under armed serialisation (with no ladder a cell is a cache, not a
	// rung): health checks and every mode without --target are unchanged byte for
	// byte. 0 = off.
	float reenter = 0.5f;
	// THE QUOTA HOSTS. The relevant state is not the board: it is (board,
	// off-board resources), and the once-per-turn quota of the carrying effects
	// (Wolf's fusion, Chick's rename) is visible neither from the digest, nor from
	// the atoms, nor from the cell. Measured consequence: the "short path" cell
	// representative is systematically the state that has NOT paid, i.e. the goal
	// conjunction on the vector, dead in play. The activation count of those hosts
	// ALONG THE PATH (MSG_CHAINING, no core patch) enters the cell KEY: two states
	// with the same vector but different quotas stop sharing a representative.
	// Canonical codes; at most 12 tracked, one bit each (spent / fresh).
	std::vector<uint32_t> quota_hosts;
	// At equal progress the archive score prefers FRESH quotas (4 bits of the
	// score, eviction and re-entry tournament). False by default: wired in a
	// single place with quota_hosts.
	bool quota_fresh_pref = false;
	// THE DOMAIN IN TURNS (--turns, default 1 = the historical behaviour byte for
	// byte). At 2, the line crosses the OPPONENT's turn: our only decisions there
	// are the quick windows (Omega's second rip, the guard), and the opponent
	// passes on everything. Necessary as soon as a --resolve requires a resolution
	// that lives in the opposing turn, as benchmark B's reference does.
	uint32_t max_turns = 1;
	// THE "NO NEGATION ON ONE'S OWN CARDS" DISCIPLINE (same family as
	// --no-activate/--no-chain: a chosen line CONSTRAINT, not a quality pruning, so
	// rule 2 is safe). Pairs (canonical code, desc) of the deck's declared NEGATION
	// effects (NEGATE/DISABLE categories of the operator table; no name is compiled
	// in); desc 0 = every effect of the card. Null or empty = off. The cut only
	// applies to NON-forced chain windows whose last link belongs to the target
	// player.
	const std::vector<std::pair<uint32_t, uint64_t>>* self_negate = nullptr;
	// THE "GUARD RESOURCE" DISCIPLINE (--guard-keep, same family as the above).
	// Canonical codes of the cards a guard CLAUSE rests on and whose protection is
	// spent by ACTIVATING them: a clause "Zalen@field + Junk Signal@hand" only
	// covers while Junk Signal's once-per-turn is intact, so a second copy in hand
	// makes the presence atom hold over a dead card. Activating such a card from
	// the main phase is removed from the enumeration at every decision where the
	// guard, evaluated over the clauses that do NOT mention it, does not hold.
	// Chain windows are untouched: activating it THERE is the protection itself.
	// Pure function of the duel state, so nothing to undo on backtrack.
	const std::vector<uint32_t>* guard_keep = nullptr;
	// THE SELF-REFINING LADDER. A bare run has no reference line: when the frontier
	// STAGNATES (no gain in sp_max for `refine_after` measured rollouts; the
	// threshold is PRINTED), the next return to the rung aims at the BEST cell,
	// solves the LP there (the marking is read from the duel), and the places of
	// its residual x* not yet served become the sub-rungs of a sub-ladder: the cell
	// key extends by one level beyond the door, and the return to the rung and the
	// tournament work unchanged on the refined ladder. ONE level to start with,
	// measured. 0 = off.
	uint32_t refine_after = 0;
	// THE PATH QUOTAS IN THE LP (red-black). At refinement time, the observed uses
	// of the quota hosts (quota_uses, the same bookkeeping as the cell key) enter
	// the LP's capacities: h and the sub-ladder become honest about what the path
	// has ALREADY spent. Only bites at RefineLadderHere; an infeasible LP there
	// gives up on refining, it cuts no line (a soft failure mode, intended as long
	// as the theorem-2 walk with quotas is the judge).
	// False by default.
	bool quota_h = false;
	// The balance model (root of the LP), lent by main.cpp; null otherwise.
	const class BalanceModel* balance = nullptr;
	// Strict novelty: only a fact never seen counts (see NoveltyTable).
	bool novelty_strict = false;
	// Cut greedy rollouts too. MEASURED and off by default: the table is shared
	// between rollouts, and a rollout re-walking the same beginning dies
	// `patience` decisions before it could deviate, 2/8 instead of 6/8 on the
	// transplantation case. That is Rollout-IW's failure mode without a tree: the
	// tree variant works around it, but NRPA gives more here for less complexity.

	// --- NRPA (rollouts under a learned policy) ---
	// Nesting level and iterations per level. The cost of a level-L call is
	// iters^L rollouts.
	int nrpa_level = 2;
	uint32_t nrpa_iters = 24;
	float nrpa_alpha = 1.0f;
	// GNRPA bias of a repertoire move: the slot intended for a prior.
	float nrpa_bias_known = 1.5f;
	// Domain hints (--hint): canonical cards whose moves (summon, activate,
	// position) receive an additional bias. It is the channel through which the
	// player's knowledge ("Zalen satisfies Hot Red Abyss's condition") enters the
	// sampling without remodelling anything.
	std::vector<uint32_t> hint_cards;
	float hint_bias = 2.0f;
	// BIAS AGAINST ENDING THE TURN.
	//
	// THE FACT: 683 044 rollouts out of 683 044, ONE HUNDRED PER CENT, end with
	// "turn", none by constraint and none by guard. Ending the turn is
	// IRREVERSIBLE and is drawn uniformly among ~10 idle choices: surviving 32 idle
	// decisions is worth `0.9^32 ~ 3 %`. So the solver is not searching a space of
	// 331 decisions, it is searching the first five.
	//
	// This is a BIAS, never a pruning (rule 2): the choice stays drawable, its mass
	// drops. At 3.0 survival goes from 3 % to ~85 %.
	float phase_w = 0.0f;
	// Partial persistence of the policy between restarts: the weights are attenuated
	// by this factor instead of restarting from zero. Restarts with a virgin policy
	// forgot the sublines learned (measured: 6/8 with the same cards missing, run
	// after run). 0 = the previous behaviour.
	float nrpa_restart_keep = 0.5f;
	// Best sequence shared between workers (null = no sharing).
	NrpaShared* nrpa_shared = nullptr;
	// INITIAL policy of RunNrpa (null = virgin): the merged policy of the rollout
	// phase is the starting point for the finisher's rollouts rooted on the
	// backtrack states; without it, every root would relearn from scratch.
	const NrpaPolicy* nrpa_init = nullptr;
	// ADAPTATION replay of the corpus: decision sequences recorded on the solution
	// lines (LiftPolicyRun), adapted into the policy BEFORE the first rollout,
	// `nrpa_adapt_passes` passes over each line. The injection point is that of the
	// weight prior (initial policy of the first restart), so what varies is the
	// FORM of the injection alone, a per-move bonus against a discriminative
	// gradient.
	// 0 = inactive.
	const std::vector<NrpaRun>* nrpa_adapt_runs = nullptr;
	uint32_t nrpa_adapt_passes = 0;
	// TWO-LEVEL policy: retention of the contextual level, s = n/(n+k). Negative k
	// = mechanism OFF (previous behaviour, the contextual cell is neither read nor
	// written). k = 0: the context takes over from the first update; large k: much
	// evidence is needed.
	float ctx_shrink = -1.0f;
	// PATH CONDITIONING (MCPS 2510.06381). 0 = off: the contextual level keeps the
	// semantic descriptor (placed, hand), byte-for-byte the previous behaviour.
	// k > 0: a decision's context becomes the mixed sum of the moves played over
	// the first k decisions of the line, frozen beyond that.
	//
	// WHY A TRUNCATION, when MCPS takes the whole path. The retention
	// s = n/(n+shrink) already handles depth on its own: a deep context never seen
	// again has n = 0, hence s = 0, hence an exact fallback to the global weight.
	// What it does not handle is MEMORY: without truncation the table holds one
	// (move, path) pair per visited node, i.e. hundreds of millions. The truncation
	// bounds the table by the number of prefixes of depth <= k, i.e. by the width
	// of the TOP of the tree, which is narrow, and which is precisely where the
	// life-or-death information sits.
	// --- HEAD BANDIT WITH PERMUTATION STATISTIC (--qhat) ---
	// Depth, in RECORDED decisions, over which MCPS's selection rule replaces
	// softmax sampling: argmax of val = (n Q + n^ Q^) / (n + n^), weights
	// proportional to the sample sizes, no bias hyperparameter. 0 = OFF,
	// byte-for-byte the previous behaviour (no window allocated, no node created,
	// no reward computed).
	//
	// Why a depth and not the whole tree: a complete tree would need one cell per
	// visited node, i.e. hundreds of millions. The top of the tree is NARROW
	// (measured: 207 cells cover the first six decisions) and that is where the
	// life-or-death information sits, since an opening decides the viability of the
	// whole line.
	uint32_t qhat_depth = 0;
	// Size of the sliding window of rollouts, PER WORKER. The paper takes 10 000;
	// we pay it sixteen times. The summary prints the measured memory.
	uint32_t qhat_window = 4096;
	// Visits after which a NON-ROOT node freezes its permutation statistic and
	// becomes the reference of its subtree (the rho of GRAVE/MCPS). The root is
	// kept incrementally and never frozen.
	uint32_t qhat_rho = 32;
	// Cap on the bandit's nodes, per worker. At the cap, no new node is created
	// and the decision falls back on softmax sampling: the mechanism degrades
	// towards the previous behaviour instead of inflating the memory of sixteen
	// workers.
	size_t qhat_max_nodes = 65536;
	// Cap on the entries of the contextual level, per worker. Past it, EXISTING
	// cells keep being updated but no new one is created: the mechanism degrades
	// towards the global weight instead of inflating the memory of sixteen
	// workers. The size reached is printed.
	size_t ctx_max = 262144;
	// SAMPLING TEMPERATURE (GNRPA, arXiv:2003.10024): the logits are divided by tau
	// before the softmax. It is the only known lever that acts on MASS rather than
	// on RANKING, and the measurement says the ranking was already good (the corpus
	// move ranks first in 96 % of cases on a virgin policy) while the mass is what
	// is missing (44 % mean probability, i.e. 0.44^160 over a whole line). tau < 1
	// concentrates, tau = 1 = the previous behaviour. The adaptation uses the SAME
	// temperature, otherwise the gradient would not be that of the sampled
	// distribution.
	float nrpa_temp = 1.0f;
	// GNRPA with limited repetitions (arXiv:2401.10420): number of times the best
	// sequence may be RE-FOUND (same MATERIAL score; the novelty part of the score
	// decreases on every replay, so strict equality would never happen) before the
	// level is stopped. Re-finding the same line signals convergence BEFORE
	// stagnation (8 iterations with no progress) admits it. 0 = the old behaviour,
	// stagnation alone.
	// PHS* (arXiv:2103.11505, the same paper as LTS): weight of the distance to the
	// goal in the finisher's cost, cost = log(d+1) + levin_h * h(n) - log pi(n),
	// with h = missing target cards + missing resolutions at the expanded node
	// (inherited by its children). Pure Levin (0) is blind to the goal: it climbs
	// back up deep backtracks without preferring the branches that rip or that
	// place. 0 = pure Levin.
	float levin_h = 1.0f;
	// sqrt-LTS (arXiv:2412.05196): re-root the finisher's search at every HINT. The
	// Levin cost d(n)/pi(n) is replaced by the rooted cost lambda/pi(n ; n_k) where
	// n_k is the nearest hint ancestor, so the probability restarts from 1 at every
	// hint instead of multiplying over the whole line. Our hint is already computed
	// at every node: the number of target board cards placed CHANGES.
	//
	// The min over ancestors in the paper reduces here to a single term, and that
	// is EXACT rather than an approximation: with uniform weights on the hints, for
	// n_j preceding n_k, lambda/pi(n;n_j) >= (1/pi(n_k|n_j)) * lambda/pi(n;n_k) >=
	// lambda/pi(n;n_k), so the NEAREST hint ancestor always minimises. One lambda
	// per node is therefore enough.
	//
	// Forecast measured before implementation (ForecastSearchCost): monolithic
	// bound 10^26 to 10^60 expansions depending on the policy, decomposed bound
	// over the 18 segments 10^5.8 to 10^12.5.
	// false = the previous Levin cost, bit for bit.
	bool levin_reroot = false;
	// sqrt-LTS-H (arXiv:2605.30664 section 3.2, the best of the paper's three
	// rerooter designs on CraftWorld, the domain closest to ours: materials
	// consumed, dead end by consuming a piece, 306 224 expansions under LTS against
	// 2 515 under sqrt-LTS-H).
	//
	// The HARD rerooter of `levin_reroot` requires a discrete EVENT (the number of
	// target cards placed changes). On the Lunalight case that event does not
	// happen before step 76 out of 108: the rerooter has nowhere to bite, and
	// sqrt-LTS degenerates into LTS over the 70 % of line that precedes it. The
	// HEURISTIC rerooter does not have that defect, since it gives a weight to
	// EVERY node:
	//
	//     w_t = exp(-alpha * h(n_t) / h(root))          (Eq. 7 of the paper)
	//
	// with h our distance to the goal (missing target cards + missing resolutions).
	// alpha is an INVERSE temperature: small, the weights look alike and the
	// rerooter is conservative; large, the mass concentrates on the nodes that have
	// visibly progressed. 0 = off.
	//
	// The cost becomes c(n) = min_{n_t < n} (1/w_t) * c^r_{n_t}(n), the paper's min
	// (Eq. 3) instead of the hint ancestor alone. We keep it in O(1) through a
	// two-term recurrence (see RunLevin): extend the best ancestor, or re-root.
	float reroot_h = 0.0f;
	// --- finisher replays ---
	// The profile established that RunLevin's cost is the REPLAY of the path at
	// every jump of the queue (80 Process per expansion), not the expansion of the
	// children. Two mechanisms attack it, orthogonal and each switchable; they
	// change neither the reachable space nor a node's Levin cost, only the ORDER of
	// ties and the STARTING POINT of the replays. Control: benchmark 0, backtrack 0
	// = 42 expansions, b=0, EXHAUSTED.
	//
	// FULL dive stack: push an arena level at EVERY replayed chain node, not only
	// at the expanded node. Page traffic is roughly the same (a segment's dirty
	// pages are SPREAD between the pushes, and only the common hot pages are logged
	// several times) but the stack then holds the whole branch: the shared ancestor
	// found is the real branch point, not the last surviving expanded node.
	//
	// ON BY DEFAULT: on benchmark 0, rj (replayed edges per chain) goes from ~12/12
	// to ~1.5/14, Process per expansion from 54.5 to 12.8, +92 % expansions at
	// equal time, with the same 42 expansions / b=0 / EXHAUSTED on the control and
	// the same best per root (the extraction order is UNCHANGED, only the speed
	// changes). --no-dive-full turns it off.
	bool dive_full = true;
	// LIFO tie-break in the queue: at EQUAL Levin cost, extract the node queued
	// LAST (the child of the node just expanded). Wins nothing on its own: exact
	// ties are rare (log-probabilities differ per node), rj does not move, and the
	// changed order visits more expensive states (90.4 against 70.4 us/Process) for
	// -20 % expansions; neutral when combined with dive_full. Off by default.
	// MERGED pop (Arena::PopToAndRestore) when returning to the shared ancestor:
	// each hot page is copied once instead of once per level. ON BY DEFAULT: exact
	// equivalence checked on benchmark 0 (42/b=0/EXHAUSTED, same best), unit
	// Restores 4.36 -> 0.58 per expansion, arena 47 -> 37 % of the phase, +4.5 %
	// expansions at equal time. --no-merged-pop turns it off.
	bool merged_pop = true;
	// POST-GOAL RECOVERY in the finisher (opt-in): under --optimize, a goal node of
	// RunLevin keeps going instead of being treated as Dead. The goal is already
	// recorded by GoalCheck, and further decisions can reduce the burned cards
	// without touching the board (the semantics of anytime rollouts). Without
	// --optimize: no effect (the finisher stops at the goal, the historical
	// behaviour).
	bool finisher_post_goal = false;
	// MACRO EDGES IN THE FINISHER (full adoption of Alikhasi & Lelis 2410.11262).
	// Macros live in PolicyRollout, yet it is RunLevin that CONVERTS
	// (32 lines to the board through the finisher, 16 solutions written). Here an
	// applicable macro becomes an EDGE of the Levin tree: it costs log 1/pi_macro
	// like any edge, advances by k decisions for ONE unit of depth, and a missing
	// key makes it ABORT, i.e. a dead edge, exactly as in a rollout. Nothing is
	// removed from the space: all the atomic edges are still there beside it.
	//
	// THE CONTROL CHANGES, AND THAT IS LEGITIMATE. Benchmark 0 can no longer be
	// read as "42 expansions": macro edges compress the paths, so the expansion
	// counts and the probabilities of the atomic edges (whose denominator grows)
	// move BY CONSTRUCTION. The control becomes "same best per root, no solution
	// lost, EXHAUSTED still EXHAUSTED". Turned off, the path is bit for bit the
	// previous one.
	bool finisher_options = false;
	// --- options ---
	// Catalogue of macros offered to the NRPA sampling (null = off, byte for byte
	// the previous behaviour). See OptionCatalog.
	//
	// CAUTION: under ONLINE mining this pointer CHANGES mid-run. It is then always
	// equal to the worker's `options_hold.get()`; that shared_ptr is what
	// guarantees the lifetime. Never copy it anywhere but inside a rollout.
	const OptionCatalog* options = nullptr;
	// ONLINE MINING: non-null = the worker pours its best lines into the living
	// corpus and periodically buys the catalogue back. See OnlineOptions. Null =
	// byte for byte the previous behaviour (the catalogue, if any, is the startup
	// one and never moves).
	OnlineOptions* options_online = nullptr;
	// Worker identity, for the living corpus's per-worker quota. No effect when
	// `options_online` is null.
	uint32_t worker_id = 0;
	// The form used is log(d+1) + levin_h*h - log pi, not the paper's PHS*,
	// (d + h)/pi: a weighted-A*-style weighting without the paper's guarantee, and
	// that is an ACKNOWLEDGED choice.
	// --- recipe graph ---
	// Non-null: the graph is FED by the observed summons, and `h` becomes the
	// distance over that graph instead of the count of missing cards. Null: the
	// previous behaviour, byte for byte. The graph belongs to the Search; the
	// pointer allows a pre-learned one to be supplied.
	RecipeGraph* recipes = nullptr;
	// Weight of the recipe distance in `h`. 0 = the graph is FED and MEASURED but
	// does not enter the cost: that is the mode that quantifies what the graph
	// would be able to say before letting it decide.
	float recipe_h = 0.0f;
	// --- REPETITION PROBE: the instrument BEFORE the mechanism ---
	//
	// WHAT IT SEPARATES, and why nothing else does. The solver's wall is "reaching
	// a subgoal consumes what the next one needs". Two OPPOSITE failures produce
	// the same `best_overlap`:
	//   - the second copy is NEVER ATTEMPTED (the material was there, the policy
	//     did not go for it): the fix is in the SAMPLING;
	//   - it is ALWAYS LOST (the chain was already consumed when the first one
	//     landed): the fix is in `h`.
	// The two call for opposite work. This probe tells them apart.
	//
	// HOW. Per watched --summon-min entry: (1) the histogram of summons PER
	// ROLLOUT; (2) at the FIRST summon, the recipe distance to ONE MORE copy, with
	// the fresh copy removed from availability, otherwise the distance would answer
	// "0, it is there" instead of "how much for the next one". The reference is the
	// same distance from the rollouts' STARTING STATE: distance preserved =
	// material preserved.
	//
	// COST: one zone sweep per rollout that REACHED the first summon (never per
	// decision). Requires `recipes`; without the graph the probe would only have
	// the histogram, and saying so beats a mute figure.
	bool probe_repeat = false;
	// CARDS OBSERVED BY THE PROBE, with no constraint at all (`--watch`).
	//
	// WHY THEY EXIST. The probe could only count the cards of --resolve /
	// --summon-min, and `--resolve` IS A DISGUISED HINT: the code gives it the hint
	// bias automatically (`cfg.hint_cards.push_back(req.code)`), on top of a
	// +resolve_weight gradient and a goal requirement. Measuring "does the solver
	// find it on its own?" with a counter that, in order to exist, points at the
	// answer, makes no sense.
	//
	// `--watch` does NOTHING but count: no constraint, no gradient, no bias. It is
	// the condition for a BARE run to be measurable. At most 4 (counters packed as
	// 4 x 16 bits).
	std::vector<uint32_t> probe_watch;

	// --- LEARNED LANDMARK GRAPH --------------------------------------------
	// Learned offline from the resolved plans, shared READ-ONLY by every worker:
	// `Build()` is called once, before the first thread, and nothing writes to it
	// afterwards. No lock, so it is usable on the hot path. That is the difference
	// in kind from the recipe graph, which learns DURING the run and pays a mutex.
	const LandmarkGraph* landmarks = nullptr;
	// Weight in the ROLLOUT SCORE, in units of `Heuristic` (one target card placed
	// is worth 100, one required resolution `resolve_weight`). It is the wiring
	// that matters: 99 % of the work is in the rollouts, and it is their score the
	// NRPA adaptation follows. 0 = measure without weighting.
	float landmark_weight = 0.0f;
	// Weight in the FINISHER's `h` (same entry point as `recipe_h`). Separate from
	// the above so that only one of them need move.
	float landmark_h = 0.0f;

	// --- FOUR LEVERS AGAINST THE ARITY LAW -----------------------------------
	//
	// THE MEASURED FACT: the frequency of a summon collapses with its number of
	// materials, 42 525 rollouts for a 2-material Fusion, 2 073 at 3, ZERO as soon
	// as a material is NAMED. The solver fails at N = 1. Mechanical cause:
	// `material = common x 100 + ...` where `common` counts the target cards
	// PRESENT, a term that is null while none is placed. Nothing rewards
	// APPROACHING a payable Fusion.
	//
	// Four flags, four SEPARABLE mechanisms, all off by default. Each is judged on
	// its own: never change two factors at once.

	// (1) RESOLVED ASSIGNMENT (--assign). Subset prompts ALSO emit the two extreme
	// subsets in the sense of the recipes (see EnumOptions::assign_useful). Attacks
	// the LEXICOGRAPHIC truncation of ForEachSubset, which makes the right subset
	// not rare but ABSENT.
	// Refresh period of the list of useful codes, in rollouts: the graph learns
	// during the run, and the list must follow it without taking its lock at every
	// decision.
	bool assign = false;
	// (1b) ASSIGNMENT BIAS (`--assign-bias`), the mechanism the diagnosis POINTS AT
	// rather than the one that had been guessed.
	//
	// THE MEASUREMENT THAT COMMANDS IT. The ZONE PRESENCE probe shows that the door
	// is wide open and the material never arrives:
	//     Masquerade activated ........... 21.4 % of rollouts (225 942 times)
	//     Leo Dancer in the GRAVEYARD .... 202 rollouts out of 579 000, i.e. 0.035 %
	// And Leo Dancer is OFFERED 14 433 times in a selection prompt (the one asking
	// which Lunalight to send from the extra to the graveyard) and chosen 202
	// times: **1.4 % conversion**. So it is neither a state problem (the
	// opportunity arises) nor a door problem (it opens): it is the CHOICE that is
	// not steered.
	//
	// WHAT THE FLAG DOES: adds this weight to the logit of the choices that engage
	// a code the RECIPE GRAPH designates as a material of a missing target card
	// (`snap_useful`, already computed by `--assign`).
	//
	// HOW IT DIFFERS FROM `--hint`: the list is not written by hand and is not the
	// literal target. It is DERIVED from the observed and seeded recipes, so it
	// names the MATERIALS, which are precisely what the target does not say. An
	// earlier goal-directed bias failed because it biased towards Liger, a move
	// that does not exist yet; this one biases towards what has to be done BEFORE.
	float assign_bias = 0.0f;
	// OPERATOR BIAS (--op-bias).
	//
	// WHAT IT BIASES, AND HOW IT DIFFERS FROM `--assign-bias`. The latter
	// designates MATERIALS and acts mostly on SELECTION prompts ("which Lunalight
	// to send to the graveyard"). This one designates OPERATORS, the cards whose
	// backward decomposition requires PRESENCE ON THE FIELD for an acquisition edge
	// to exist, so it acts on "what to play".
	//
	// WHY THAT PROMPT, AND THE ARITHMETIC. The resolved plan counts 143 FREE
	// decisions, only 32 of which are `IDLECMD`. The policy tops out at 66 %
	// agreement per decision; 91 % would be needed to hope for a success over 143.
	// But `0.66^32 ~ 1.7e-6`, i.e. of the order of one success per 90 s run. A plan
	// does not replace sampling: it conditions the distribution over the 32
	// decisions that matter, and that is the only lever the arithmetic allows.
	//
	// A plan is a BIAS, never a pruning. Nothing is removed from the space; if the
	// planner is wrong, the sampler still covers everything.
	// The list is `snap_operators`, derived from `snap_reqs` by a DECLARATIVE
	// criterion: a NAMED requirement whose zone is the FIELD. An acquisition edge
	// is the only thing that posts one, since a material is taken from the
	// graveyard, the hand or the reserve, never from "in play".
	float op_bias = 0.0f;
	// GRADIENT TRUNCATION AT THE SCORE PEAK (--adapt-to-peak).
	//
	// See NrpaRun::peak_steps for the defect it fixes. Opt-in, so that turning it
	// moves one factor only; when false, the path is byte for byte the other one.
	bool adapt_to_peak = false;

	// (2) HINDSIGHT (--hindsight w). Andrychowicz et al., NeurIPS 2017: a failure
	// relabelled by the goal it ACTUALLY reached. Our measured gold is right there:
	// the solver places Perfume Dancer 42 525 times per run and throws it all away
	// because it was not the target, since `AdaptRun` only reinforces the SINGLE
	// best sequence. Here every EXTRA DECK monster actually summoned becomes a
	// substitute goal, and the best line reaching it undergoes the NRPA gradient at
	// `w x alpha`. No corpus, no network: the signal already exists and is being
	// discarded.
	// 0 = off, byte for byte the previous behaviour.
	float hindsight = 0.0f;
	// Substitute goals kept at most (memory: one complete NrpaRun each, per
	// worker).
	size_t hindsight_k = 16;

	// (3) RECIPE DISTANCE IN THE ROLLOUTS (--recipe-w). `RecipeDistance` existed
	// for a long time but was only called in `RunLevin`, i.e. nowhere near where
	// 99 % of the work happens. It is carried here over a cheap path (local copy of
	// the graph, zones queried only when a requirement names them).
	// As PROGRESS rather than as distance: `w x 1000 x (d0 - d)`, where d0 is the
	// distance at the rollout's first decision. Same convention as the landmarks,
	// and a score that stays positive (a rollout's score is a MAX initialised at 0;
	// a negative term would kill it).
	// Refresh period of the graph snapshot, in rollouts.
	uint64_t recipe_snap_period = 2048;

	// (4) BACKWARD SERIALISATION (--backward). Retro* / AO*: a summon is an AND
	// node and its materials are its children; the arity, fatal FORWARD, becomes a
	// DECOMPOSITION backwards. `RecipeGraph::Expand` returns the list of
	// subproducts in build order; the number of those already placed enters the
	// PARTITION of the novelty table. So the table reopens at every brick built,
	// before any target card is on the field, i.e. exactly where the current
	// serialisation (target cards placed) is flat. Serialized IW / BFWS, applied to
	// the learned decomposition instead of the literal goal.
	bool backward = false;
	// Weight of a required resolution (--resolve) in the rollouts' gradient. At 100
	// (one target card), the 8/8 lines WITHOUT a rip win the adaptation race
	// against the partially ripping lines (measured: 850 k rooted rollouts, zero
	// rip converted); at 250, a rip is worth 2.5 placed cards and a line with 5
	// cards + 2 rips beats a mute 8/8 line.
	float resolve_weight = 250.0f;
	// GOAL BY INCLUSION: `target` included in `here` instead of `here == target`.
	// Every posted entry must be present, and extra cards are tolerated.
	//
	// THE DEFAULT DEPENDS ON WHERE THE TARGET COMES FROM, and that is the whole
	// fix.
	// - Target CAPTURED from a real line (benchmark B, `--start` without
	//   `--target`): EXACT EQUALITY. The reference board carries its own continuous
	//   spells, so requiring it identically makes sense.
	// - Target POSTED by hand (`--target`): INCLUSION, by default. Posting a target
	//   means "I want those cards", not "those cards and an empty field around
	//   them".
	//
	// WHAT THE OLD DEFAULT COST, measured: it made benchmark A UNSATISFIABLE. A
	// posted target is built from a clean slate and every `--target` goes to the
	// MZONE, so its S/T zone is EMPTY, i.e. it REQUIRES a field with no spell and
	// no trap. But the opening hand is three Fire Formation - Tenki, a CONTINUOUS
	// spell (type 0x20002) that stays on the field as soon as it is activated or
	// set, and `--resolve` adds Lunalight Masquerade, also in the S/T zone. No line
	// could satisfy that goal, and goal-only mode wrote ZERO solutions with a
	// ceiling always described as "all the codes are there, the goal differs only
	// in the DETAIL", the detail being the S/T cards the game forces one to leave.
	// Player's ruling: the S/T are a bonus, and for Lunalight only the monsters
	// count in the end. `--target-exact` restores equality.
	bool goal_subset = false;
	// Go-Explore archive: number of DISTINCT states (cell = complete board) kept
	// with their path during the search. 0 = no archive.
	size_t archive_k = 0;
	// QUOTA PER PROGRESS LEVEL. The archive is ranked by a score that SATURATES in
	// goal-only mode (every root at r4 on benchmark A) and it then fills with
	// END-OF-LINE states: 37 roots, EXHAUSTED in 0 to 13 expansions. The quota
	// reserves a share of the cells for each (resolutions, cards placed) pair, i.e.
	// gives the archive back its nature as a COVERING. false = the previous
	// behaviour, byte for byte (a global leaderboard).

	// --- repair: semantic resynchronisation ---
	// digest -> index into yrp.responses (see LiftRefLine). Null = no
	// resynchronisation: after a deviation, the recorded suffix stays blind (the
	// measured state: swapping summons #4/#5 is unreachable up to k=12).
	// jusqu'a k=12).
	const std::unordered_map<uint64_t, size_t>* ref_digests = nullptr;
	// plan_key per answer index (see LiftRefLine): the WINDOWED repertoire. After a
	// first deviation, a move the reference plays within `repair_window` decisions
	// of the current point is free. Never on the pure prefix: the invariant "0
	// deviations finds the reference again" is preserved.
	const std::vector<uint64_t>* ref_keys = nullptr;
	uint32_t repair_window = 24;

	// --- transposition table shared between workers (lazy SMP) ---
	// Null = a private table per worker (the previous behaviour).
	SharedTT* shared_tt = nullptr;

	// NO MEMOISATION AT ALL (neither shared nor private). Set for the ZERO
	// deviation pass, which follows a SINGLE path: memoising cannot save work
	// there, it can only cut, and a reference line that makes a no-op round
	// trip revisits a state it has already been in. Its own table then killed
	// the control pass that exists to find it back. Off everywhere else: the
	// cut is right for the SEARCH, wrong for the REPLAY.
	bool no_memo = false;

	// --- line constraints ---
	// n-th summon (1-based, normal + special; Nibiru's count, flips excluded) ->
	// admissible CANONICAL codes. A summon at a constrained index whose card is not
	// in the list PRUNES the branch: the constraint reduces the space instead of
	// filtering after the fact. Conditional semantics: a line that never reaches
	// the n-th summon is not at fault.
	std::map<uint32_t, std::vector<uint32_t>> summon_constraints;
	// Guard: from the `guard_after`-th summon on, at every OPPONENT response window
	// (where, and only where, Nibiru can land) at least one clause must hold.
	// Transient gaps in mid-resolution (the guard leaves as material while its
	// replacement arrives) are NOT faults: the opponent cannot act there. That is
	// what makes the guard playable; requiring it at every decision would forbid
	// converting one guard into another.
	uint32_t guard_after = 0;
	std::vector<GuardClause> guard_clauses;
	// Turning the guard off: past a certain point the threat no longer exists, and
	// a deck that empties the opponent's hand (handrip) has no more Nibiru to fear.
	// The guard is not required at windows where the opponent's hand holds at most
	// this many cards. -1 = never turned off.
	int guard_opp_hand_release = -1;

	// Minimum number of effect resolutions per card. Counts the ACTIVATIONS
	// (MSG_CHAINING): playing solo nothing negates a chain, so an activation counts
	// as a resolution, filtered by activation zone (see ResolveReq). A MINIMUM
	// constraint: it cannot prune mid-line (the future can still fulfil it), it is
	// checked AT THE GOAL. A conforming board without the resolutions is not a
	// solution, and the search goes on. At most 4 cards (counters packed as 4 x 16
	// bits in a path uint64).
	std::vector<ResolveReq> resolve_min;
	// MATERIAL constraint: when this (canonical) card is summoned, its materials,
	// the MSG_MOVE entries marked REASON_SYNCHRO|REASON_MATERIAL in the same
	// resolution, must include at least one card whose attribute intersects the
	// mask ("Chaos Angel summoned with a LIGHT monster"). A violation cuts the
	// branch: a failed summon cannot be undone.
	std::vector<std::pair<uint32_t, uint32_t>> material_req;

	// Initial counters, for a search starting in the MIDDLE of a line (finisher:
	// replay a prefix then dig from its state). Without them, the summon
	// constraints and the turn cut-off would count from zero although the prefix
	// has already summoned and started turn 1.
	uint32_t initial_summons = 0;
	uint32_t initial_turns = 0;
	uint64_t initial_resolved = 0;   // packed resolution counters

	// --- anytime COST objective (lexicographic optimisation) ---
	// The search no longer stops at the first solution: each solution tightens the
	// bound, the kept set is bounded by replacing the WORST (lexicographic cost:
	// burned, then actions, then decisions, deduplicated by path), and the NRPA
	// goal score becomes lexicographic, so a cheaper line has a better score and
	// the adaptation pulls towards it (the Montparnasse recipe, arXiv:2505.02110).
	// Rollouts CONTINUE past the goal: further decisions can REDUCE the burned
	// cards (real recoveries cut both ways), and every re-reaching of the board is
	// re-recorded when it is cheaper.
	bool anytime = false;
	// Burned bound (B&B): a state whose CURRENT burned count exceeds
	// best_known_burned + burn_slack is cut (rollouts only). Burned cards are NOT
	// monotonic along a line (recoveries), so the slack absorbs the recoveries and
	// is MEASURED on the reference (the "max burned mid-line" report). >= 255 =
	// bound inactive.
	uint32_t burn_slack = 6;
	// Seed of the bound: the best burned count already known BEFORE the search
	// (solutions from earlier phases). 0 = none.
	uint32_t burn_limit = 0;
	// Burned bound SHARED between workers: the GLOBAL best burned count
	// (UINT32_MAX = none). Without it, a worker finding 19 cuts nothing for the
	// other fifteen; each improvement is published (CAS min) and the cut reads the
	// global bound + burn_slack at every test (a relaxed load, negligible next to
	// the two Count() calls of the test). Null = the local bound alone.
	std::atomic<uint32_t>* shared_burn = nullptr;

	// --- ALTERNATIVE goals (opponent test, --fire) ---
	// Boards also accepted at the goal: the target board MINUS each subset of the
	// cards sacrificed to answer the threat (player's ruling: answering Nibiru with
	// Zalen consumes Junk Signal, and may cost one more building piece). Null = the
	// previous behaviour. The pointer must outlive the search.
	const std::vector<BoardKey>* target_alts = nullptr;
};

struct Solution {
	std::vector<std::vector<uint8_t>> responses;
	uint32_t actions = 0;      // summons + activations (tier 2)
	uint32_t decisions = 0;    // tier 3
	uint32_t burned = 0;       // cards in the graveyard + banished (tier 1)
	uint32_t hand_left = 0, deck_left = 0, extra_left = 0;
	// Reached through the ALTERNATIVE goal (cfg.target_alt, the board without the
	// sacrificed card) rather than through the complete board.
	bool alt = false;
};

// REPETITION PROBE: one set of counters per watched --summon-min entry.
// Additive across workers: everything is summed, min'd or max'd there, never
// averaged over averages.
struct RepeatProbe {
	uint32_t code = 0;
	// Rollouts reaching >= k summons of the product, k = 1..5. So `reached[0]` is
	// "at least one", and the gap reached[0] -> reached[1] is the question.
	uint64_t reached[5] = { 0, 0, 0, 0, 0 };
	// At the FIRST summon: recipe distance to ONE MORE copy.
	uint64_t more_n = 0;
	double more_sum = 0.0;
	uint32_t more_min = 0xffffffffu, more_max = 0;
	// Rollouts where that distance is <= the reference (material PRESERVED) and
	// where it is > (chain CONSUMED). This is the line that returns the verdict.
	uint64_t more_kept = 0, more_lost = 0;
	// Distance to the REST of the target at the same instant: the consumption axis,
	// which also applies to a goal with no repetition (benchmark B).
	double rest_sum = 0.0;
	// Reference: the same distances from the rollouts' STARTING STATE.
	// 0xffffffff = never measured.
	//
	// IT IS RE-MEASURED, and that is a fix rather than a refinement. The recipe
	// graph is ENRICHED during the run (every observed summon enters it), so a
	// reference taken on the very first rollout would be compared, a thousand
	// seconds later, against a distance computed on a graph that is no longer the
	// same. The bias goes towards a false positive: the arrival distance rises
	// because the graph knows more, not because the material was consumed. So the
	// reference is re-taken periodically, and `d0_samples` says how many times; at
	// 1, read it as a single figure.
	uint32_t d0 = 0xffffffffu;
	uint32_t rest0 = 0xffffffffu;
	uint64_t d0_samples = 0;
	// Decisions left after the first summon; without it, "the second never arrives"
	// is indistinguishable from "the rollout was over". And the DEPTH of that first
	// summon, which is benchmark B's question: at which decision was the material
	// consumed?
	uint64_t after_sum = 0;
	uint64_t first_depth_sum = 0;
	// Does the graph know a recipe for this card? Without this flag a distance of 1
	// is ambiguous between "floor, nothing to say" and "one summon is enough", two
	// opposite readings (see RecipeGraph::Knows).
	bool known = false;
	// OFFER PROBE: THE DECOMPOSITION OF THE ARITY LAW.
	//
	// `reached[0] == 0` ("never summoned") covers two opposite failures, and they
	// have to be separated:
	//   - offer_rollouts == 0: the card is NEVER OFFERED. The core only lists a
	//     Fusion when its materials are payable at that instant, so the failure is
	//     in the STATE (a decreasing h, backward search).
	//   - offer_rollouts > 0 and reached[0] == 0: it is OFFERED and never taken.
	//     The failure is in the SAMPLING (assignment, hindsight).
	// `offer_steps` counts DECISIONS, `offer_rollouts` counts ROLLOUTS: their ratio
	// says whether the opportunity is unique or repeated.
	uint64_t offer_steps = 0, offer_rollouts = 0;
	// PROMPT TYPES that offered it, one bit per `prompt_type`, same mechanism as
	// `forced_default_prompts`. THE RESERVATION THAT MAKES THEM INDISPENSABLE:
	// "present in a prompt's pool" is NOT "summonable". An effect that REVEALS the
	// extra deck, a discard, a search all list the card too. Without the breakdown,
	// a 5 % offer rate would read as "the game offered it to you 33 000 times and
	// you refused", when perhaps none of those 33 000 times was a summon.
	uint64_t offer_msgs = 0;
	// ACTIVATIONS: rollouts having ACTIVATED the card at least once, and the total
	// count. A card that opens a route (Wolf, Masquerade) is not summoned; without
	// this counter, "never summoned" did not say whether the solver had even tried
	// the door.
	uint64_t act_rollouts = 0, act_total = 0;
	// ZONE PRESENCE: the missing part, and the only one that talks about STATES.
	//
	// The three other parts count EVENTS (summon, activation, offer). But the combo
	// is played on states: "Leo Dancer IN THE GRAVEYARD", "three Lunalight
	// available". `Lunalight Leo Dancer` is never summoned NOR activated; its role
	// is to REACH the graveyard so it can be banished there as a material. Without
	// this counter, "never summoned" did not say whether the card had even reached
	// the zone where it serves.
	//
	// A rollout counts ONCE per zone reached. Index: see ZoneSlot.
	uint64_t zone_rollouts[6] = { 0, 0, 0, 0, 0, 0 };
	// CHOICES TAKEN that engage the card, paired with the offers. This is THE
	// usable judge: "Leo Dancer in the graveyard" counts ~200 events over a run, so
	// inter-run noise (a measured factor of 6 at equal seed) swamps it, whereas the
	// offer -> choice CONVERSION counts 14 433 opportunities in the same run, i.e.
	// the same phenomenon measured seventy times more finely.
	// `taken_steps / offer_steps` is the quantity any choice-steering mechanism has
	// to move.
	uint64_t taken_steps = 0;
	// YES/NO: how many times the prompt was OFFERED for this card, and how many
	// times "yes" was taken. On benchmark A the gap between the two IS the
	// bottleneck: Masquerade's discard is optional, and refusing it closes access
	// to the graveyard for the rest of the turn.
	uint64_t yn_steps = 0, yn_yes = 0;
	// OFFERS COUNTED BY PROMPT TYPE. The mask alone was not enough, and the first
	// reading went wrong because of it: Leo and Liger were "offered in 36 500
	// rollouts", which read as "the game offered them and you refused", when the
	// DECISION count was 36 507 for 36 503 rollouts, i.e. ONE prompt per rollout,
	// always the same one, and no trace of a placement. A pool containing all FOUR
	// of the deck's Fusions at once is the extra deck read whole, not a list of
	// payable summons.
	//
	// Indices: see kOfferMsgs. Six types are enough; beyond that we would pay 64
	// counters per card for prompts that carry no summon at all.
	uint64_t offer_by[7] = { 0, 0, 0, 0, 0, 0, 0 };
};

// Prompt types tracked by name in the offer probe. The order is that of
// RepeatProbe::offer_by and it is shared by the solver and the printing.
//   0 IDLECMD  : normal/special summon or set, from the hand or the extra
//   1 CARD     : generic selection; this is the ambiguous one
//   2 UNSELECT : incremental material selection
//   3 SUM      : tributes, Synchro materials
//   4 CHAIN    : chain window
//   5 POSITION : the card is PLACED, the only direct proof of a summon
// Zones tracked by the PRESENCE probe, in the order of
// RepeatProbe::zone_rollouts. The entry expects an ALREADY normalised zone (see
// NormalizeZone). 0 hand | 1 field | 2 graveyard | 3 banished | 4 extra | 5 deck
inline int ZoneSlot(uint8_t normalized) {
	switch(normalized) {
	case 0x02: return 0;
	case 0x0c: return 1;
	case 0x10: return 2;
	case 0x20: return 3;
	case 0x40: return 4;
	case 0x01: return 5;
	default:   return -1;
	}
}
inline const char* ZoneSlotName(int slot) {
	static const char* kNames[6] = { "main", "field", "grave", "bannie",
									 "extra", "deck" };
	return (slot >= 0 && slot < 6) ? kNames[slot] : "?";
}

// Prompt whose choices are SUBSETS: `Choice::card` is only an APPROXIMATE
// identity there (the subset's first code). That is exactly the boundary the
// `hint_seen` breakdown has to follow; without it, the counter mixes the flag's
// effect with the run's quality and judges nothing any more.
inline bool IsSubsetPrompt(uint8_t msg) {
	return msg == MSG_SELECT_CARD || msg == MSG_SELECT_TRIBUTE ||
		   msg == MSG_SELECT_SUM;
}

inline int OfferSlot(uint8_t msg) {
	switch(msg) {
	case MSG_SELECT_IDLECMD:       return 0;
	case MSG_SELECT_CARD:
	case MSG_SELECT_TRIBUTE:       return 1;
	case MSG_SELECT_UNSELECT_CARD: return 2;
	case MSG_SELECT_SUM:           return 3;
	case MSG_SELECT_CHAIN:         return 4;
	case MSG_SELECT_POSITION:      return 5;
	// YES/NO: this is where the discard that unlocks the graveyard materials on
	// benchmark A is decided. The prompt had no identity at all until yes/no
	// identity was added, so the probe could not see it.
	case MSG_SELECT_EFFECTYN:
	case MSG_SELECT_YESNO:         return 6;
	default:                       return -1;
	}
}

struct SearchStats {
	uint64_t nodes = 0;             // states expanded
	uint64_t transpositions = 0;    // merges by the table
	// STATES ALREADY SEEN BUT WITH A SMALLER BUDGET, hence RE-EXPLORED. The table
	// stores `disc + 1` and only cuts when the existing entry is at least as large;
	// a state seen again with more budget is therefore re-expanded. Counting only
	// the CUT (`transpositions`) would not say whether the mechanism pays. Counted
	// on the PRIVATE table path; the shared
	// (lossy) table does not tell the two cases apart.
	uint64_t tt_reexplored = 0;
	uint64_t dead_ends = 0;         // answers rejected by the core
	uint64_t terminals = 0;
	// Branches cut by a CEILING (depth in decisions, action budget) rather than by
	// the space itself. While this was zero everywhere (it was written nowhere), a
	// search whose branches had all been shaved off by the bound displayed
	// "EXHAUSTED", i.e. a proof of absence. Non-zero, the word becomes "EXHAUSTED
	// UNDER BOUND".
	uint64_t edges_skipped = 0;
	// Subset enumerations TRUNCATED by max_subsets. Same nature as `edges_skipped`
	// (legal answers the search never saw) but on the enumerator's side.
	uint64_t subsets_capped = 0;
	// Branches dropped because another worker held the point's token (ClaimTable
	// partitioning). Without this counter, a locked partition is indistinguishable
	// from an empty search space, which is what happened.
	uint64_t claim_denied = 0;
	// Prompts the enumerator could not open, reduced to THE default answer. This is
	// not pruning: it is a slice of the space that never existed. The mask records
	// which message types are concerned, so the report can name the prompt instead
	// of only saying there are some.
	//
	// TWO DISTINCT EVENTS, and one counter cannot carry both. Incrementing a single
	// counter BEFORE trying `DefaultResponse` also counts, in `dead_ends`, the
	// branches that do not survive: "reduced to THE default answer" would then name
	// branches that were in fact DELETED, the same event would be counted twice,
	// and "90 forced prompts and 90 dead ends" would read as two concordant facts
	// when it is one.
	//   forced_default: a default answer EXISTS, the branch survives, reduced.
	//   forced_killed : no default answer, the BRANCH DIES. That is the case of the
	//                   three ANNOUNCE_*, structurally out of reach.
	uint64_t forced_default = 0;
	uint64_t forced_killed = 0;
	uint64_t forced_killed_prompts = 0;
	uint64_t forced_default_prompts = 0;
	// Number of DISTINCT states reached at each depth: the curve that decides
	// whether "exhaustive" is a realistic word.
	std::vector<uint64_t> distinct_by_depth;
	std::vector<uint64_t> expansions_by_depth;
	// Best approach to the target board encountered: number of target cards
	// gathered at the same time, and number of monsters placed. When the search
	// finds nothing, that pair distinguishes "look further" from "this deck cannot
	// chain".
	uint32_t best_overlap = 0;
	uint32_t best_monsters = 0;
	// The board actually obtained at the moment of the best approach. Without it,
	// "5 of the 8 cards" does not say WHICH are missing, which is the only
	// information one can act on.
	std::vector<uint32_t> best_board;
	// The answers LEADING to that best state. It is the finisher's raw material:
	// replay that path and search exhaustively from its state, where sampling can
	// climb but misses the last step.
	std::vector<std::vector<uint8_t>> best_path;
	// The field of the best state, IN DETAIL (positions, materials, counters). When
	// the 8 codes are there but the goal does not fire, this is where WHAT differs
	// can be read; a count cannot be fixed, a detail can.
	std::vector<QueriedCard> best_mzone, best_szone;
	// --- novelty pruning ---
	uint64_t novelty_novel = 0;     // states having produced an unseen atom
	uint64_t novelty_stale = 0;     // mute states (no new atom)
	uint64_t novelty_cuts = 0;      // branches cut, patience exhausted
	size_t novelty_atoms = 0;       // final size of the atom table
	// --- constraints ---
	uint64_t constraint_cuts = 0;   // branches cut by --summon
	uint64_t guard_cuts = 0;        // branches cut by the guard (--guard)
	// Chain options removed by --no-self-negate. The mechanism's LIVENESS: at zero
	// with the flag armed, either the window never came up or the wiring is dead,
	// and both can be read.
	uint64_t self_negate_cuts = 0;
	// Main-phase activations removed by --guard-keep. Same liveness reading as
	// above: zero with the flag armed means the window never came up, or the
	// wiring is dead.
	uint64_t guard_keep_cuts = 0;
	// --- anytime cost objective ---
	uint64_t burn_cuts = 0;         // rollouts cut by the burned bound
	// sqrt-LTS: number of expanded nodes that RE-ROOTED the search (a hint landed
	// there). Without this counter, an inactive rerooting would be
	// indistinguishable from a useless one.
	uint64_t reroots = 0;
	// --- recipe graph ---
	// Summons OBSERVED and poured into the graph. At zero, the graph is empty and
	// the recipe distance is exactly today's flat `h`: the mechanism is then inert,
	// and one has to know that BEFORE concluding anything.
	uint64_t recipes_seen = 0;
	// Sum of the recipe distances evaluated, and their count: their ratio is the
	// MEAN `h` the graph produces. Compared with |missing target|, it says whether
	// the landscape really got deeper or stayed flat.
	double recipe_h_sum = 0.0;
	uint64_t recipe_h_count = 0;
	// --- landmark graph ---
	// Achievements STILL MISSING, summed over the evaluations, and their count.
	// Their ratio is the MEAN landmark `h`. Compared with the total number of
	// landmarks learned, it says whether the landscape really deepens: an `h` stuck
	// to the total means the search achieves NOTHING, an `h` stuck to zero that the
	// landmarks are too easy and do not guide.
	// Without these two counters, a live `h` is indistinguishable from an inert
	// one.
	double landmark_h_sum = 0.0;
	uint64_t landmark_h_count = 0;
	// --- LIVENESS OF THE FOUR MECHANISMS -------------------------------------
	// A mechanism whose work and cost are not measured ends up tuned blind: a
	// `mean h` printed by a function that does NOT cover the rollout phase, i.e.
	// not where the flag acts, says nothing about whether `h` is decreasing.
	//
	// (3) recipe distance evaluated IN THE ROLLOUTS: sum, count, and the summed
	// reference d0. `d0_sum / count` against `sum / count` says whether the
	// distance REALLY decreased, and not only that it was computed.
	double rec_roll_sum = 0.0, rec_roll_d0_sum = 0.0;
	uint64_t rec_roll_count = 0;
	// Graph snapshots taken by the workers, and the size of the last one: at 0
	// products, the three mechanisms are LIVE AND INERT.
	uint64_t recipe_snaps = 0;
	uint64_t snap_products = 0, snap_useful = 0, snap_backward = 0;
	// LIVENESS OF THE OPERATOR BIAS (--op-bias). Without it, a comparison measures
	// the same arm twice: a mechanism can be inert while printing that it acts.
	// `offered`: decisions where at least one designated operator could be offered;
	// `taken`: those where the move played engaged one.
	uint64_t op_bias_offered = 0, op_bias_taken = 0;
	uint64_t op_bias_listed = 0;   // size of the list at the last snapshot
	// (4) subproducts already built, summed over the evaluations. Related to
	// `snap_backward`, it is the decomposition depth actually reached.
	double backward_sum = 0.0;
	uint64_t backward_count = 0;
	// (2) hindsight: substitute goals kept, and the adaptations they triggered. At
	// 0 goals, the mechanism saw nothing go by.
	uint64_t hindsight_goals = 0, hindsight_adapts = 0;
	// DISTINCT BOARDS (cfg.count_boards): how many explored states fall back onto
	// the same board. The states/boards ratio is the price of the transposition
	// key's fineness.
	size_t boards_entries = 0, boards_loose = 0, boards_codes = 0;
	// The same, restricted to the STABLE points (idle prompt). Elsewhere we are in
	// the middle of a resolution: the board is not formed yet, and two states with
	// the same board are legitimately distinct there (chain in progress, processor
	// stack). Without that restriction, the states/boards ratio mixes the price of
	// the key's fineness with the natural number of intermediate instants, and
	// proves nothing.
	uint64_t states_idle = 0;
	size_t boards_idle = 0;
	// ATTRIBUTION of the inflation, counted AT THE SAME NODES (idle points): how
	// many DISTINCT values each component of the key takes. Read together they say
	// which one blows the table up and by how much. `d_zones` is the floor (the
	// visible game state), `d_full` is the current key, and the gap between
	// `d_zones` and `d_payload` isolates the payload.
	size_t d_zones = 0, d_payload = 0, d_proc = 0, d_full = 0;
	// The same game state with the columns CONFLATED. `d_zones / d_zsort` is the
	// exact price of the column in the transposition key.
	size_t d_zsort = 0;
	// BREAKDOWN OF EVERY EXPANDED NODE: the GLOBAL attribution, the one that was
	// missing when the column was fixed on the strength of a measurement restricted
	// to idle points. `forced`: a single legal answer, so nothing to decide.
	// `idle`: a stable decision point. `multi`: the rest (selections, chain windows
	// with several options).
	uint64_t nodes_forced = 0, nodes_idle = 0, nodes_multi = 0;
	// Forced moves PLAYED INLINE (cfg.elide_forced): they cost neither depth, nor a
	// table entry, nor an arena snapshot.
	uint64_t elided = 0;
	// Broken arithmetic in the sqrt-LTS cost. `levin_overflow`: a term went to
	// infinity (hu/pi with pi floored at 1e-30, or exp(-seg_logpi) beyond ~709).
	// `reroot_by_overflow`: among the counted `reroots`, how many happened because
	// the comparison flipped for purely NUMERICAL reasons. `lam_saturated`: lambda
	// pinned at 1e300, after which log(lam-1) is constant for the whole descent and
	// the best-first degenerates. Non-zero, the arm is to be THROWN AWAY, not
	// interpreted; without these three counters, a mechanism that switched itself
	// off read like a mechanism that lost.
	// Children GENERATED by the finisher. Related to `reroots`, it says what
	// FRACTION of the edges re-roots, the only way to distinguish "the rerooter
	// bites sometimes" from "it re-roots everywhere", two opposite failures that
	// `reroots` alone does not separate.
	uint64_t levin_children = 0;
	uint64_t levin_overflow = 0;
	uint64_t reroot_by_overflow = 0;
	uint64_t lam_saturated = 0;
	// h at the ROOT OF THE CURRENT SEARCH: the denominator of Eq. 7. It is ~1 in
	// the finisher (the starting state is already at 7/8 cards) and not |target|;
	// without printing it, the alpha dial does not say at which scale it was swept.
	// Zero = soft rerooter off.
	double h_root = 0.0;
	// --- finisher replays ---
	// The profile showed that RunLevin's cost is the REPLAY of the path at every
	// jump of the queue, not the expansion of the children. These three counters
	// measure what the dive stack ABSORBS and what it lets through:
	// `replay_decisions` = replayed answers (SetResponse on already expanded
	// nodes); `replay_chain` = cumulated length of the chains of the extracted
	// nodes (the denominator: at 0 replayed per chain, the stack absorbs
	// everything); `dive_misses` = extractions restarted from the ROOT for lack of
	// a common ancestor on the stack.
	uint64_t replay_decisions = 0;
	uint64_t replay_chain = 0;
	uint64_t dive_misses = 0;
	uint64_t goal_hits = 0;         // goal reached (re-reaches included)
	// --- repair ---
	// Semantic resynchronisations: states whose digest found a point FURTHER ALONG
	// the reference, making the recorded suffix readable again after a deviation.
	uint64_t resyncs = 0;
	// --- rollouts ---
	uint64_t rollout_count = 0;
	uint64_t turn_cuts = 0;         // rollouts stopped at the turn change
	uint64_t nrpa_adapts = 0;
	// --- PER-ROLLOUT PROGRESS PROFILE ---------------------------------------
	// The closed form: with unequal blocks, the cost of a serialised run is
	// Sigma b^(l_i), DOMINATED by the largest gap between rungs (5.9^12 ~ 10^9
	// where 5.9^8 ~ 10^6). This profile is the instrument for the question: where
	// do the rollouts stop on x*'s ladder? A single peak = a nameable LOCK; a
	// spread = it is ARITY that kills, and only a finer grain can help.
	// `sp_final[k]`: rollouts whose MAX SerialProgress reached k units.
	// `sp_at_sum`: sum of the decision indices of the last progress.
	// `sp_lines`: rollouts measured. The counter only lives when serialisation AND
	// novelty are both on, because that is where SerialProgress is computed.
	// 72 cells: with the reserve-departure slices, the total served reaches ~65 on
	// the reference line.
	uint64_t sp_final[72] = {};
	uint64_t sp_at_sum = 0;
	uint64_t sp_lines = 0;
	// LIVENESS OF THE RETURN TO THE RUNG (--reenter). `reenter_rollouts`: rollouts
	// that actually restarted from a cell; `reenter_fail`: prefix replays that died
	// on the way (MSG_RETRY or end of duel; a cell's path MUST replay from the
	// root, so a failure here is a defect to look at, not noise);
	// `reenter_base_sum`: sum of the starting rungs (its mean says WHICH rung the
	// rollouts restart from).
	uint64_t reenter_rollouts = 0;
	uint64_t reenter_fail = 0;
	uint64_t reenter_base_sum = 0;
	// LIVENESS OF THE REFINEMENT. `refine_done`: this worker re-serialised from its
	// frontier cell (0 or 1); `refine_subrungs`: sub-rungs posted; `refine_gate`:
	// base rung from which the key extends; `refine_top_hits`: states archived
	// beyond the gate (the refined ladder was actually WALKED, not merely posted).
	uint64_t refine_done = 0;
	uint64_t refine_subrungs = 0;
	uint64_t refine_gate = 0;
	uint64_t refine_top_hits = 0;
	// Hindsight breakdown: substitute goals committed with a tracked quota already
	// spent, versus with every quota fresh. The measured question: does hindsight
	// reinforce the early fusions that spend Wolf's quota on Tiger?
	uint64_t hindsight_quota_spent = 0;
	uint64_t hindsight_quota_fresh = 0;
	// LIVENESS OF --adapt-to-peak: steps NOT REMOVED from the gradient because they
	// followed the score peak. At zero the mechanism is INERT and no search judge
	// concerns it: either the lines peak at their last step, or the flag is off.
	uint64_t peak_truncations = 0;
	// --- options ---
	// The mechanism's liveness triptych: macros CHOSEN by the sampling, decisions
	// ABSORBED by their scripted steps (the exponent gain is absorbed/chosen), and
	// macros ABORTED on the way (a key not offered by the prompt; when this
	// dominates, the catalogue does not match the prompts the search meets).
	uint64_t macro_taken = 0;
	uint64_t macro_absorbed = 0;
	uint64_t macro_aborted = 0;
	// CONTEXTUAL level: size reached by the table, and whether the cap bit. Under
	// path conditioning (MCPS), this is THE reading that says whether the mechanism
	// has room to learn or degrades towards the global level; a conditioning whose
	// table saturates is no longer the paper's.
	size_t ctx_entries = 0;
	bool ctx_capped = false;
	// --- HEAD BANDIT (--qhat): the mechanism's liveness ---
	// `qhat_decisions` = decisions taken by the MCPS rule (at zero, the bandit
	// never decided: zero depth, or the node cap reached immediately).
	// `qhat_playouts` = rollouts poured into the window, `qhat_reward_sum` their
	// cumulated reward (their ratio says how high the run works: a mean reward
	// stuck at zero means the window holds only indistinguishable failures and Q^
	// can separate nothing).
	// `qhat_first` = decisions taken AT THE FIRST decision of the rollout, the one
	// the diagnosis aims at.
	uint64_t qhat_decisions = 0;
	uint64_t qhat_first = 0;
	uint64_t qhat_playouts = 0;
	double qhat_reward_sum = 0;
	uint64_t qhat_fallback = 0;   // decisions handed back to the softmax (cap)
	size_t qhat_nodes = 0;
	size_t qhat_codes = 0;
	size_t qhat_bytes = 0;
	// Visibility of the hints (--hint): in how many states a hinted move was LEGAL,
	// and how many times it was taken. hint_seen = 0 means the problem is not
	// sampling but LEGALITY: the core never offers the summon, the materials are
	// not there.
	//
	// BROKEN DOWN BY PROMPT TYPE, and it had to be: the single counter had THREE
	// causes at once, the `card_on_select` behaviour (which makes `Choice::card`
	// non-zero on every SELECTION prompt), the run's quality (a line reaching
	// states where the hinted card is playable sees it often) and plain work
	// volume. Measurements that established it: 4 with the flag at ONE worker,
	// 64 617 WITHOUT the flag at sixteen. A counter with three causes can judge
	// none of them.
	//   *_exact: IDLECMD, CHAIN, POSITION, BATTLECMD, where `card` REALLY
	//            designates the card engaged. The only part that talks about a move.
	//   *_sel  : SELECT_CARD, SELECT_TRIBUTE, SELECT_SUM, where `card` is only the
	//            first code of a subset. This is the part that MOVES with the
	//            subset-prompt identity.
	uint64_t hint_seen = 0, hint_taken = 0;
	uint64_t hint_seen_exact = 0, hint_seen_sel = 0;
	// Rollouts having reached >= k required resolutions (k = 1..4). THE handrip
	// diagnosis: separates "the policy never rips" (sampling,
	// resolve_reached[0] > 0) from "the rip is never legal/possible here" (game,
	// resolve_reached[0] = 0).
	// Only four cells, although the required total can go up to 15 (up to four
	// --resolve entries, each with its min_count): with `--resolve X:3 --resolve
	// Y:3`, the histogram returns the SAME numbers whether a rollout reaches 4 or 6
	// resolutions, i.e. exactly the discrimination it exists for.
	// `resolve_overflow` counts the crossings beyond the 4th; non-zero, the
	// histogram is truncated and must be read as such.
	uint64_t resolve_reached[4] = { 0, 0, 0, 0 };
	uint64_t resolve_overflow = 0;
	// Best peak CONDITIONED on COMPLETE resolutions: how far do the lines that did
	// ALL the rips climb? If they top out far from the board while the mute lines
	// do 8/8, the two goals are probably incompatible in RESOURCES, which is a game
	// question (or an arithmetic relaxation to prove), no longer a search one.
	uint32_t best_overlap_ripped = 0;
	// THE BEST JOINT LINE: the path of the lexicographic max (resolutions done,
	// target cards placed). The `best_overlap_ripped` peak was only a COUNTER: the
	// lines crossing the lock died with the run (47 rollouts at 3 rips in one run,
	// none kept). The path is written at the end of the run (best_joint_*.yrp) and
	// re-injected through --approach. Only lives under --resolve
	// (resolve_total > 0).
	uint32_t best_joint_rp = 0, best_joint_overlap = 0;
	std::vector<std::vector<uint8_t>> best_joint_path;
	// Repetition probe (--probe-repeat): one entry per --summon-min card.
	RepeatProbe rep[4];
	double ms = 0;
	bool exhausted = false;         // space exhausted within the given bounds
	bool hit_time_limit = false;
	bool hit_node_limit = false;
	// MEMORY safeguard of the finisher (4 M queued nodes), distinct from the cap on
	// expanded nodes: without it a root truncated by memory is typographically
	// indistinguishable from a normal root.
	bool hit_memory_limit = false;
	// The worker's arena overflowed: everything measured AFTER that is wrong
	// (Restore() does not restore the objects that went to the host heap). This is
	// not a ceiling, it is an invalidation.
	bool arena_poisoned = false;
};

// End-of-search status, in one word, for the root tables. "EXHAUSTED" is a
// PROOF OF ABSENCE and is only written when no bound bit: a non-zero
// `edges_skipped` downgrades it to "EXHAUSTED UNDER BOUND", and the two
// ceilings are named.
inline const char* SearchOutcome(const SearchStats& st) {
	// First: a poisoned arena invalidates EVERYTHING else, including any
	// "EXHAUSTED".
	if(st.arena_poisoned)   return "!! ARENA POISONED";
	if(st.hit_time_limit)   return "budget";
	if(st.hit_memory_limit) return "memory";
	if(st.hit_node_limit)   return "nodes";
	// One non-exhausted case remains: the search stopped on its solution quota.
	// Without this word it displays as an EMPTY column, indistinguishable from an
	// unexplained stop.
	if(!st.exhausted)       return "quota";
	// A truncated enumeration removes LEGAL answers from the space: it strips
	// "EXHAUSTED" of its value as a proof, exactly as a ceiling does.
	return (st.edges_skipped || st.subsets_capped) ? "EXHAUSTED UNDER "
														"BOUND"
												   : "EXHAUSTED";
}

class Search {
public:
	Search(Duel& duel, Arena& arena, const Replay& yrp, const SearchConfig& cfg);

	// Depth-bounded exhaustive enumeration. The duel must be positioned at the
	// search's starting point.
	void Run(const BoardKey& target);

	// Search GUIDED towards the target board.
	//
	// The exhaustive search does not get past ~50 decisions where the reference
	// line has 276, so reaching the board means steering the descent. At each node
	// we evaluate the children (advance / measure / restore, which the snapshot
	// makes affordable) and descend first towards the one that places the most
	// target board cards.
	void RunGuided(const BoardKey& target);

	// BOUNDED-DISCREPANCY search, seeded on the reference line.
	//
	// This is the strategy the problem calls for: a better line for the SAME board
	// is almost surely a small perturbation of the known line, e.g. not activating
	// a card, taking another material. So we follow the reference and allow at most
	// `discrepancies` deviations, trying to pick the recorded line up again after
	// each one.
	//
	// Useful property: at zero deviations the search replays the reference and
	// therefore necessarily finds it. The solver can no longer return "no solution"
	// without that being a defect.
	void RunRepair(const BoardKey& target, uint32_t discrepancies);

	// TRANSPLANTATION: reach the same board from ANOTHER duel, i.e. another deck,
	// another hand, another seed.
	//
	// Here the reference line no longer exists: its answers designate nothing in
	// this duel. What remains is its INTENT, recorded by LiftPlan. The plan is
	// treated as a REPERTOIRE, not as a schedule. Two decks do not ask the same
	// questions in the same order: requiring a sequential alignment falls off at
	// the first unfamiliar question, and that is measured, not assumed. A move the
	// reference played, anywhere in its line, is therefore free; any other move
	// costs a deviation. The budget then measures what has to be INVENTED on top of
	// the repertoire.
	//
	// Unlike RunRepair, nothing guarantees a solution exists: a deck may simply not
	// have the board's cards.
	void RunTransplant(const BoardKey& target, const std::vector<PlanStep>& plan,
					   uint32_t discrepancies);

	// GREEDY ROLLOUTS. The bounded-discrepancy search is wide and short: it
	// exhausts the repertoire at zero deviations without ever getting near the
	// board, then explodes at the first invented move. But the board is ~300
	// decisions away, so what is needed is DEPTH.
	//
	// A rollout descends in one go to the end of the turn, choosing at each step
	// among the best children according to the heuristic, with a random share that
	// varies from one rollout to the next. A thousand rollouts visit a thousand
	// distinct deep lines where a depth-first descent locks itself into a single
	// subtree. The repertoire acts as a prior: a move the reference played starts
	// with a bonus.
	void RunRollouts(const BoardKey& target, const std::vector<PlanStep>& plan,
					 uint32_t count, uint64_t seed);

	// NRPA (Cazenave): rollouts under a LEARNED POLICY instead of child
	// evaluations. One weight per move code (our plan_key is exactly that) and an
	// adaptation towards the best sequence at each level. The repertoire enters as
	// a bias (GNRPA), not as a bonus: additive, it wrongly steered the descent
	// towards "end of turn".
	//
	// Two distinct gains: the policy concentrates the rollouts, and doing without
	// child evaluation (advance/measure/restore on EVERY child) makes each decision
	// several times cheaper.
	void RunNrpa(const BoardKey& target, const std::vector<PlanStep>& plan,
				 uint64_t seed);

	// FINISHER: Levin Tree Search (arXiv:2103.11505) from the current position, a
	// COMPLETE best-first search ordered by the cost d(n)/pi(n), where pi(n) is the
	// product of the NRPA policy's softmax probabilities along the path. The number
	// of expansions before finding a solution is bounded by the policy's quality:
	// it is the principled needle-finder the rollouts will never be. Sampling
	// misses a rare sequence; enumeration ordered by the policy finds it in order
	// of its probability.
	//
	// FORCED moves (opponent windows, single-candidate selections) are played
	// inline and cost neither depth nor probability (a forced decision costs zero,
	// the same principle as in repair). A node is replayed from the root by its
	// decision path: a 0.05 ms restore then a replay of the suffix, which is
	// Go-Explore's "return", already paid for.
	// Uses cfg.initial_* (the search starts in the middle of a line).
	void RunLevin(const BoardKey& target, const std::vector<PlanStep>& plan,
				  const NrpaPolicy& policy);

	const SearchStats& Stats() const { return stats; }
	const std::vector<Solution>& Solutions() const { return solutions; }
	// Go-Explore archive collected during the search (cfg.archive_k > 0).
	const std::vector<ArchiveEntry>& Archive() const { return archive; }
	// ARCHIVE SEEDING (--carry). The cells of a previous round re-enter BEFORE the
	// first rollout: the return to the rung starts from yesterday's frontier
	// instead of rebuilding it.
	// PRECONDITION: the entries' paths must be replayable from THE ROOT of this
	// search (same template; true between rounds of one command, NEVER for a
	// finisher rooted on a prefix, so only seed the searches of the rollout phase).
	// A path that does not replay is counted by reenter_fail, not silent. Returns
	// the number of entries seeded (capped at cfg.archive_k, best first).
	size_t SeedArchive(const std::vector<ArchiveEntry>& seed);
	// Policy learned by RunNrpa, exported at the end of the run: it guided the
	// rollouts, and it then guides the finisher (RunLevin).
	const NrpaPolicy& LearnedPolicy() const { return final_policy; }
	// BANDIT PROBE: the statistics of the ROOT (s = {}), i.e. of the rollout's
	// first decision. It is the instrument required BEFORE any measurement, since
	// the corpus agreement curve is blind to Q^. Empty when --qhat is off.
	std::vector<BanditProbe> RootBandit() const;

private:
	enum class Step { Prompt, Ended, Rejected };

	// NRPA policy: one weight per plan_key, softmax sampling. (PolicyStep /
	// NrpaRun live at namespace level: the best sequence is shared between workers
	// through NrpaShared.)
	using Policy = NrpaPolicy;

	Step StepToPrompt();
	uint64_t Digest() const;
	// `prompt_depth` counts the PROMPTS crossed, `depth` the DECISIONS kept. The
	// two coincide without `cfg.elide_forced`; with it, the first advances on
	// forced moves and the second does not. The ChoiceList pool is indexed by
	// `prompt_depth`: two successive nodes can share the same `depth`, and sharing
	// one buffer would make them overwrite each other.
	void Descend(uint32_t depth, uint32_t actions, uint32_t prompt_depth = 0);
	bool DescendGuided(uint32_t depth, uint32_t actions, uint32_t turns,
					   uint32_t summons, uint64_t resolved);
	bool DescendRepair(uint32_t depth, uint32_t actions, size_t ref_index,
					   uint32_t disc, uint32_t stale, uint32_t turns,
					   uint32_t summons, uint64_t resolved);
	bool DescendTransplant(uint32_t depth, uint32_t actions, uint32_t disc,
						   uint32_t stale, uint32_t turns, uint32_t summons,
						   uint64_t resolved);
	// Checks the summons of the last StepToPrompt against the constraints, `before`
	// being the number of summons already made on this path. Returns false when a
	// constraint is violated (the branch must die).
	bool SummonsOk(uint32_t before) const;
	// Opponent intervention window under an active guard: returns true when the
	// guard does not hold and the branch must be cut.
	bool GuardCut(const BoardKey& here, uint32_t summons);
	// One rollout, from the current position to the end of the turn. Returns true
	// when the target board was reached.
	bool Rollout(uint64_t& rng);
	// One rollout under the policy: no child evaluation, the choice is drawn
	// according to exp(weight + bias). Fills `run` for the adaptation.
	void PolicyRollout(uint64_t& rng, const Policy& pol, NrpaRun& run);
	// RETURN TO THE RUNG (--reenter): with probability cfg.reenter, replays the
	// path of a UNIFORMLY drawn archive cell and arms the prefix counters for the
	// PolicyRollout that follows. No effect (and no cost) when serialisation is not
	// armed or the archive is empty. On replay failure: arena.Restore() and a
	// normal rollout from the root.
	void ReenterMaybe(uint64_t& rng);
	// The policy is copied only once per level call (copying at every rollout would
	// mean a table of thousands of entries per rollout).
	double Nrpa(int level, const Policy& pol, NrpaRun& best, uint64_t& rng);
	// Upper level: adapts the PERSISTENT policy (by reference, for persistence
	// across restarts) and exchanges the best sequence with the other workers
	// (cfg.nrpa_shared).
	double NrpaTop(Policy& pol, NrpaRun& best, uint64_t& rng);
	void Adapt(Policy& pol, const NrpaRun& best);
	// One round of ONLINE mining, called at the SAFE BOUNDARY (between two
	// upper-level iterations: no rollout in flight in this worker, so no
	// `active_macro` points into the catalogue). Pours `best` into the living
	// corpus, buys the catalogue back when it has changed, and re-mines when it is
	// this worker's turn. No effect when cfg.options_online is null.
	void OnlineOptionsTick(const NrpaRun& best);
	// End of a rollout under --qhat: pours the line and its reward into the sliding
	// window, backpropagates Q(s,a) over the nodes crossed, and FREEZES those
	// reaching rho visits. Called by an RAII guard, because PolicyRollout exits
	// through a dozen `return` statements, and the rollouts that DIE are precisely
	// the ones Q^ draws its signal from.
	void QhatCommit();
	// Enumerates the current prompt into `out` (opponent: "do nothing" only;
	// non-enumerable prompt: the default answer). false = dead branch.
	bool FillChoices(ChoiceList& out);
	// One ChoiceList per depth, reused between siblings and between passes: the
	// deque guarantees reference stability during the recursion.
	ChoiceList& ChoicesAt(uint32_t depth) {
		while(choice_pool.size() <= depth)
			choice_pool.emplace_back();
		return choice_pool[depth];
	}
	// Number of target board entries already in place: the distance to the goal.
	// Takes the already-computed board; that was the hot spot, two zone queries per
	// evaluated child, one of them redundant with ComputeBoardKey.
	uint32_t Heuristic(const BoardKey& here) const;
	// Distance over the recipe graph. Non-const: it records the hidden zones and
	// fills a reused buffer.
	//
	// `probe_code` non-zero (repetition probe): under THE SAME availability table
	// (one zone sweep, which is the whole cost), ALSO return in `*d_more` the
	// distance to ONE MORE copy of that product, with ONE copy present on the FIELD
	// removed from availability. Without that removal the answer would be "0, it is
	// there", which is not the question asked.
	float RecipeDistance(const BoardKey& here, uint64_t resolved,
						 uint32_t probe_code = 0, uint32_t* d_more = nullptr);
	// Repetition probe: records the distances at the first summon of the watched
	// product `i`. Does nothing without a recipe graph.
	void RepeatProbeFirst(size_t i, const BoardKey& here, uint64_t resolved,
						  uint32_t depth);
	// LANDMARKS: achievements still MISSING in this state. The field comes for free
	// from `here` (already computed by the caller); the hidden zones are only
	// queried when a landmark lives there, and that is what makes the call
	// affordable in the ROLLOUTS, where RecipeDistance is not.
	uint32_t LandmarkRemaining(const BoardKey& here);
	// Offer probe: counts the watched cards offered by the prompt just enumerated.
	// Called at BOTH enumeration points of the rollout (the elision test and the
	// normal path); a forced prompt does not come back down into the loop body, and
	// forgetting it divided the denominator by ten.
	void CountOffers(uint64_t& rep_offered);
	// --- CHEAP PATH OF THE RECIPE GRAPH ---------------------------------------
	//
	// Refreshes the local snapshot of the graph (copy under lock, every
	// `cfg.recipe_snap_period` rollouts) and derives from it the three objects the
	// mechanisms consume: the zone mask, the list of USEFUL codes and the backward
	// decomposition. Called at a rollout's safe boundary (depth 0), never mid-line,
	// because `cfg.enumeration.assign_useful` points into `snap_useful`, which must
	// not move under a prompt in flight.
	void RecipeSnapshot();
	// Recipe distance AND backward progress, on the local snapshot and with a
	// presence scan limited to the zones a requirement mentions.
	//
	// ONE FUNCTION FOR BOTH, because the cost IS the presence scan: computing it
	// twice would double the bill of the only item that matters. `backward_out`,
	// when non-null, receives the number of subproducts of the decomposition
	// already available.
	uint32_t RecipeEval(const BoardKey& here, uint32_t* backward_out);
	// HINDSIGHT: pours in the substitute goals a rollout reached. `hits`: (code,
	// depth of the first summon). Called by an RAII guard, since PolicyRollout
	// exits through a dozen `return` statements, and a rollout that DIES after
	// placing a Fusion is still a perfect example of "how to pay for that Fusion".
	// `steps`: number of PolicyStep entries recorded at the moment of the summon.
	// It is the peak SPECIFIC to that substitute goal; the rollout's `peak_steps`
	// is the OLD goal's and does not apply.
	struct HindsightHit {
		uint32_t code;
		uint32_t depth;
		uint32_t steps;
	};
	void HindsightCommit(const NrpaRun& run,
						 const std::vector<HindsightHit>& hits);
	// List of the cards the PROBE observes. `--watch` when given (pure counting, no
	// bias); failing that, the --resolve/--summon-min entries, so as not to break
	// earlier measurements. But those BIAS the sampling, and a "hint-free" reading
	// cannot rely on them.
	size_t ProbeCount() const {
		return cfg.probe_watch.empty()
				   ? (std::min)(cfg.resolve_min.size(), size_t(4))
				   : (std::min)(cfg.probe_watch.size(), size_t(4));
	}
	uint32_t ProbeCode(size_t i) const {
		return cfg.probe_watch.empty() ? cfg.resolve_min[i].code
									   : cfg.probe_watch[i];
	}
	// Best-approach tracking plus the goal test. Returns true when `here` IS the
	// target AND the resolution minimums are reached. Factors out what each
	// strategy would otherwise duplicate. In anytime mode, recording goes through
	// replacement of the worst and dedup by path; the cost of the goal stays
	// readable in goal_burned/goal_actions/goal_depth (the NRPA score reads it).
	bool GoalCheck(const BoardKey& here, uint32_t depth, uint32_t actions,
				   uint64_t resolved);
	// Current burned cards of the target player (graveyard + banished): the tier 1
	// cost, read at the goal and by the B&B bound.
	uint32_t CurrentBurned();
	// Effective burned bound: the local one, tightened by the SHARED bound between
	// workers when there is one (cfg.shared_burn, anytime only).
	uint32_t EffectiveBurnCut() const;
	// Progress towards the resolution minimums, capped: acts as a gradient for the
	// rollouts (without it, NRPA has no reason to resolve Omega twice before
	// closing the board).
	uint32_t ResolveProgress(uint64_t resolved) const;
	// Novelty pruning: observes the state and updates the silence counter. Returns
	// true when the branch must be cut. `resolved` enters the partition: the table
	// REOPENS after each required resolution (--resolve), i.e. serialisation of the
	// goal on both dimensions, cards placed AND rips done.
	// posees ET rips faits.
	bool NoveltyCut(const BoardKey& here, uint32_t depth, uint64_t resolved,
					uint32_t& stale);
	// Go-Explore archive: offers the current state (path = `path`). To be called
	// AFTER the guard and turn checks: a state that violates the guard or overflows
	// turn 1 is a starting point condemned in advance.
	void ArchiveObserve(const BoardKey& here, uint32_t depth, uint64_t resolved);
	bool BudgetExhausted() const;

	Duel& duel;
	Arena& arena;
	const Replay& yrp;
	SearchConfig cfg;
	// Sum of the required resolution minimums (see best_overlap_ripped).
	uint32_t resolve_total = 0;

	// State of the current prompt
	uint8_t prompt_type = 0;
	std::vector<uint8_t> prompt_payload;
	int prompt_player = -1;
	uint32_t actions_this_step = 0;
	// Cards summoned (normal + special) by the last StepToPrompt, in order. Raw
	// codes from the message; 0 = summoned face down (unknown).
	std::vector<uint32_t> summons_this_step;
	// Summons of the `--watch` cards seen by the last StepToPrompt, packed 16 bits
	// per entry. Strictly observational.
	uint64_t watch_this_step = 0;
	// ACTIVATIONS of the `--watch` cards seen by the last StepToPrompt, same
	// packing. Distinct from the summons: a card whose role is to OPEN a route
	// (Wolf, Masquerade) is not summoned, it is ACTIVATED.
	uint64_t watch_act_this_step = 0;
	// Zones REACHED by the `--watch` cards during the last StepToPrompt: 8 bits per
	// watched entry, one bit per ZoneSlot. Recorded from MSG_MOVE, hence at zero
	// cost, with no added zone query.
	uint32_t watch_zone_this_step = 0;
	// Resolutions (activations) of WATCHED cards by the last StepToPrompt, packed:
	// 16 bits per cfg.resolve_min entry.
	uint64_t resolved_this_step = 0;
	// A watched summon (--material) violated its material constraint during the
	// last StepToPrompt: the branch must die.
	bool material_violation = false;
	// Prefix replay in progress (return to the rung): StepToPrompt then does NOT
	// pour into the recipe graph, since re-observing the same prefix at every
	// re-entry would inflate its support in proportion to the re-entry rate.
	// re-entree.
	bool replaying = false;
	// Materials (codes) sent by the resolution in progress, for attribution to the
	// summon that follows.
	std::vector<uint32_t> recent_materials;
	// Materials of the summon in progress, WITH their source zone: the raw material
	// of the recipe graph (rule 1).
	std::vector<Requirement> recipe_materials;
	// Requirements satisfied in the current state, buffer reused between calls.
	std::vector<Requirement> recipe_present;
	// What the present entities ARE, for the cardinal requirements: an archetype or
	// a level cannot be read off a code. Filled at the same time as
	// `recipe_present` (same index) so as not to pay for a database lookup per
	// requirement evaluated.
	// A POINTER to the base row, never a copy: copying the setcode vector would
	// mean ~80 allocations per expanded node, on the path that is the finisher's
	// hottest.
	struct PresentInfo {
		const CardRow* row = nullptr;
		uint8_t zone = 0;
	};
	std::vector<PresentInfo> recipe_present_info;
	// State counter of the landmark KEYS, parallel to LandmarkGraph::Keys(). A few
	// dozen cells, reused from one decision to the next.
	std::vector<uint32_t> lm_counts;
	// Repetition probe: the d0 reference is re-taken every `kRepD0Period` rollouts,
	// never at every decision, which would be one zone sweep per decision. See
	// RepeatProbe::d0 for why it is re-sampled (the recipe graph grows during the
	// run).
	static constexpr uint64_t kRepD0Period = 2048;
	uint64_t rep_d0_next = 0;
	// Watched cards OFFERED by the last enumerated prompt, one bit per entry
	// (`--watch`). Reset by FillChoices, so valid for the current prompt and it
	// alone. Strictly observational.
	uint64_t offer_this_step = 0;

	// --- LOCAL SNAPSHOT OF THE RECIPE GRAPH ----------------------------------
	// A PER-WORKER copy, refreshed every `cfg.recipe_snap_period` rollouts. Its
	// mutex is never contended, which is what makes the evaluation affordable in
	// the rollouts, where the shared graph is not.
	RecipeGraph recipe_snap;
	uint64_t recipe_snap_next = 0;
	// Normalised zones a requirement of the snapshot mentions. The presence scan
	// only queries those: DECK and EXTRA (~55 entities) are only paid for when an
	// observed recipe really names them.
	uint8_t snap_zone_mask = 0;
	// USEFUL codes, served to the enumerator through
	// `cfg.enumeration.assign_useful`. This vector must only be rewritten at a
	// rollout's boundary, since the enumerator holds its address.
	std::vector<uint32_t> snap_useful;
	// Backward decomposition: subproducts in build order, deduplicated.
	// fabrication, dedoublonnes.
	std::vector<uint32_t> snap_backward;
	// Codes the decomposition requires ON THE FIELD: the OPERATORS to play now
	// (--op-bias). Sorted, for the hot path's binary search.
	std::vector<uint32_t> snap_operators;
	std::vector<Requirement> snap_reqs;   // buffer for RecipeGraph::Expand
	// Recipe distance at the FIRST decision of the current rollout: the reference
	// that turns a distance into PROGRESS (without it the term would be negative
	// and the rollout's score, initialised at 0, would never move).
	uint32_t recipe_base = 0;

	// --- HINDSIGHT ----------------------------------------------------------
	// Substitute goal -> best line reaching it. The score is RELABELLED: it is not
	// the line's material (which judges the old goal) but the COST of the
	// achievement, as early and as short as possible.
	struct HindsightGoal {
		double score = -1;
		NrpaRun run;
	};
	std::unordered_map<uint32_t, HindsightGoal> hindsight;
	// DISTINCT BOARDS (cfg.count_boards): empty and never touched otherwise.
	std::unordered_set<uint64_t> seen_boards, seen_loose, seen_codes, seen_idle;
	// Attribution: one distinct value per component, at the idle points.
	std::unordered_set<uint64_t> dz_set, dp_set, dpr_set, df_set, dzs_set;
	// Turn changes seen by the last StepToPrompt. The target board is the one at
	// the end of turn 1: past that it is frozen and every state explored is wasted
	// time.
	uint32_t turns_this_step = 0;
	bool saw_retry = false;
	bool ended = false;

	// Transposition table. Indexed by state, with the remaining budget under which
	// the state was solved: "explored with 8 decisions left" does not license
	// pruning when arriving with 12.
	std::unordered_map<uint64_t, uint32_t> tt;

	BoardKey target;
	uint32_t cfg_discrepancies = 0;   // deviation budget of the current pass
	const std::vector<PlanStep>* plan = nullptr;
	// Repertoire of the reference's moves: semantic identity -> rank of its first
	// appearance in the line, which acts as the visit order.
	std::unordered_map<uint64_t, size_t> plan_index;
	// Novelty table, shared by every branch of one pass: its globality is what
	// merges the "distinct but not distinctly interesting" lines that transposition
	// lets through.
	NoveltyTable novelty;
	std::vector<uint64_t> atoms_scratch;
	std::vector<Solution> solutions;
	std::vector<std::vector<uint8_t>> path;
	// `mutable`: BudgetExhausted() is const and must be able to set the poisoned
	// arena flag, which is a stop condition and not a statistic.
	mutable SearchStats stats;
	std::chrono::steady_clock::time_point start;

	// --- anytime cost objective ---
	// Cost of the last goal reached (GoalCheck), for the NRPA score.
	uint32_t goal_burned = 0, goal_actions = 0, goal_depth = 0;
	// Best lexicographic cost seen by THIS search, and the effective burned bound
	// (best burned + burn_slack; UINT32_MAX = inactive).
	uint64_t best_cost_key = UINT64_MAX;
	uint32_t best_burned_seen = UINT32_MAX;
	uint32_t burn_cut = UINT32_MAX;
	// Dedup of the solutions by path: a converged policy replays the same line
	// thousands of times, and the set keeps only one.
	std::unordered_set<uint64_t> solution_hashes;

	// Go-Explore archive (cfg.archive_k > 0): one entry per cell (complete board),
	// replacing the worst when the archive is full. The `archive_min_score`
	// threshold makes the common case (an uninteresting state) free: an integer
	// comparison, no hashing.
	std::vector<ArchiveEntry> archive;
	std::unordered_map<uint64_t, size_t> archive_cells;
	uint64_t archive_min_score = 0;
	// --- RETURN TO THE RUNG (--reenter) -------------------------------------
	// State of the current re-entered rollout, set by ReenterMaybe and consumed by
	// PolicyRollout: counters of the replayed prefix (turns, summons, resolutions,
	// actions, decisions) and the prefix itself, COPIED, because the archive can
	// evict the entry during the rollout. `path` is seeded with the prefix: a
	// solution must stay replayable from the root, and `best_path` acts as the
	// finisher's root.
	bool reenter_active = false;
	uint32_t reenter_turns = 0, reenter_summons = 0, reenter_actions = 0;
	uint32_t reenter_depth = 0;
	uint64_t reenter_resolved = 0;
	std::vector<std::vector<uint8_t>> reenter_path;
	// Size of the RESERVE (deck+extra) at the ROOT of this search: the base of the
	// departure rungs (zone 5). Set by InitSerialBase() on entry to each Run*,
	// never lazily, since the first SerialProgress arrives mid-line.
	uint32_t serial_res0 = 0;
	void InitSerialBase();
	// Uses of the quota hosts along the CURRENT path (cfg.quota_hosts, same index).
	// Exact in the rollouts (reset per rollout, re-entry prefix included); in the
	// DFS descents the counter does not go back down on backtracking, so the DFS
	// cells over-partition, which is the safe direction (extra cells, never a
	// corrupted representative).
	// corrompu).
	// ONE BIT per host: for a once-per-turn quota the distinction that matters is
	// "spent or fresh". Twelve hosts fit in the 12 free bits of the cell key, where
	// 2 bits per host capped at 6 and cut Wolf (7th in code order) off at every
	// derivation.
	uint32_t quota_uses[12] = {};
	uint64_t QuotaKey() const {
		uint64_t k = 0;
		for(size_t i = 0; i < 12; ++i)
			k |= static_cast<uint64_t>((std::min)(quota_uses[i], 1u)) << i;
		return k;
	}
	// THE SELF-REFINING LADDER: per-worker state. Stagnation is read off the max of
	// sp_final; the refinement happens AT THE NEXT return to the rung (the duel is
	// already at the cell's state there).
	bool refined = false;
	uint32_t refine_gate_sp = 0;
	std::vector<SearchConfig::SerialReq> refine_reqs;
	uint32_t max_sp_seen = 0;
	uint32_t rollouts_since_gain = 0;
	void RefineLadderHere();
	// Final policy of the NRPA run (exported for the finisher).
	Policy final_policy;
	// CONTEXTUAL level of the policy (cfg.ctx_shrink >= 0). Empty and never
	// consulted when the mechanism is off.
	NrpaResidual ctx_weights;

	// --- HEAD BANDIT WITH PERMUTATION (cfg.qhat_depth > 0) ---
	// This whole block stays EMPTY and is never consulted when the mechanism is off.
	PermWindow perm_win;
	std::unordered_map<uint64_t, BanditNode> bandit;
	// Card engaged by a move, for the PROBE only (a plan_key cannot be read; a
	// probe one cannot read is not a probe).
	std::unordered_map<uint64_t, uint32_t> bandit_code;
	// Denominator of the reward: the material of the TARGET board, a constant of
	// the problem. Computed once per search (see RunNrpa).
	double qhat_scale = 0.0;
	// Buffers for ONE rollout, reused (one allocation per rollout would be one too
	// many): nodes crossed, moves played, (node, move) pairs to backpropagate, and
	// the best reward seen along the line.
	std::vector<uint64_t> qh_nodes, qh_moves;
	std::vector<std::pair<uint64_t, uint64_t>> qh_back;
	double qh_reward = 0.0;

	// ONLINE mining: the OWNERSHIP of the catalogue this worker reads. `cfg.options`
	// is always its `.get()`. As long as this shared_ptr lives, the catalogue
	// lives, and that is what makes the buy-back safe while other workers still
	// hold the old one.
	std::shared_ptr<const OptionCatalog> options_hold;
	uint64_t options_gen = 0;

	// Buffers reused on the hot paths (one allocation per decision is one too many
	// at tens of millions of decisions per run).
	std::deque<ChoiceList> choice_pool;   // recursive searches, by depth
	ChoiceList ro_choices;                // rollouts (no recursion)
	std::vector<Message> msgs_scratch;    // StepToPrompt
	BoardKey board_scratch;               // board of the current node
	BoardKey child_board_scratch;         // board of an evaluated child
};

// Serialisation progress of a state: units served of x*'s subgoals, `packed` =
// the vector (4 bits per requirement). `res0` is the size of the RESERVE at the
// root (base of the departures, zone 5). Exposed for the finisher root probe:
// which rungs a root serves.
uint32_t SerialProgress(Duel& duel, uint8_t con,
						const std::vector<SearchConfig::SerialReq>& reqs,
						const CardDB& db, uint32_t res0,
						uint64_t* packed = nullptr);

} // namespace solver
