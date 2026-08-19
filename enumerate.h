// Enumeration of the legal answers to a MSG_SELECT_*.
//
// Every message already carries the list of legal choices: no game rule has to
// be reimplemented, only decoded and re-encoded in the shape
// OCG_DuelSetResponse expects (ProgressiveBuffer, see progressivebuffer.h).
//
// The equivalence classes applied here are the second reduction lever after the
// transposition table, and the riskiest: each one is a hypothesis. All of them
// can be turned off, and all of them are caught by the final verification,
// which replays every candidate from scratch.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "assets.h"

namespace solver {

struct Choice {
	std::vector<uint8_t> response;
	// Stable identifier of the edge, independent of the state reached. Lets the
	// transposition table be indexed by (parent state, edge) and so skip an
	// already-taken subtree WITHOUT having to materialise it.
	uint64_t edge = 0;
	std::string label;
	// Identity of the choice, for matching against a plan recorded on ANOTHER duel.
	// Equal to `edge` by default; it only differs where board equivalence already
	// ignores the detail, i.e. the choice of column. Two placements in two distinct
	// free columns lead to different states (the edge must separate them) but carry
	// out the same intent (the plan must conflate them).
	uint64_t plan_key = 0;
	// Canonical card this choice ENGAGES (summon, activate, attack, position); 0
	// when the choice engages none. Backing for domain hints (--hint): "this card,
	// try it more often".
	uint32_t card = 0;
	// Description of the effect engaged: the same u64 the message carries
	// (aux.Stringid). 0 outside chain windows. It is what gives --no-self-negate
	// its EFFECT-level precision: one card can carry a negation AND another quick
	// effect.
	uint64_t desc = 0;
	// THE IDENTITY ABOVE IS APPROXIMATE on SELECTION prompts: a subset of k > 1
	// cards has no identity, and we arbitrarily keep its first code. The `*_sel`
	// side of `hint_seen` (search.h) measures exactly the share of the bias that
	// rides on those identities; the breakdown, not a flag, is what separates the
	// two.
	// Phase change ("-> Battle Phase", "-> End Phase", "-> Main 2"). Carried by a
	// flag rather than by the label: hot paths have no label, and the GNRPA bias
	// must keep excluding those moves, since ending the turn is irreversible.
	bool phase = false;
};

// Pool of Choice objects reused between decisions. An NRPA rollout enumerates
// at EVERY decision, and rebuilding vectors and strings tens of millions of
// times per run was the first measured hot spot. Here Clear() destroys nothing:
// element responses and labels keep their capacity and Emit() reuses them.
class ChoiceList {
public:
	// Removes choice i (--no-self-negate): compaction in place, the last option
	// (decline) keeps its place at the end.
	void RemoveAt(size_t i) {
		for(size_t j = i; j + 1 < used; ++j)
			store[j] = std::move(store[j + 1]);
		if(used)
			--used;
	}
	Choice& Emit() {
		if(used == store.size())
			store.emplace_back();
		Choice& c = store[used++];
		c.response.clear();
		c.label.clear();
		c.edge = 0;
		c.plan_key = 0;
		c.card = 0;
		c.desc = 0;
		// THE POOL REUSES ITS ELEMENTS: every field added to `Choice` must be zeroed
		// here, on pain of leaking from one decision to the next. The lesson comes from
		// a real defect: an uninitialised flag cut the hint bias EVERYWHERE and the
		// case collapsed silently.
		c.phase = false;
		return c;
	}
	void Clear() { used = 0; }
	size_t size() const { return used; }
	bool empty() const { return used == 0; }
	Choice& operator[](size_t i) { return store[i]; }
	const Choice& operator[](size_t i) const { return store[i]; }
	Choice* begin() { return store.data(); }
	Choice* end() { return store.data() + used; }
	const Choice* begin() const { return store.data(); }
	const Choice* end() const { return store.data() + used; }
	const Choice& back() const { return store[used - 1]; }
	// Reduces the list to its last element (the opponent's "do nothing") by
	// swapping buffers: no content is copied.
	void KeepOnlyLast() {
		if(used > 1) {
			std::swap(store[0], store[used - 1]);
			used = 1;
		}
	}

private:
	std::vector<Choice> store;
	size_t used = 0;
};

struct EnumOptions {
	// Merges choices bearing on cards with the same code in the same place.
	bool dedup_by_code = true;
	// Caps the NUMBER of subsets enumerated (SELECT_CARD, SUM...). Past that we
	// sample the extreme sizes rather than exploding. This number caps the
	// branching factor of EVERY selection prompt.
	uint32_t max_subsets = 64;
	// Incremented when the cap above truncated an enumeration: without it a
	// truncated selection is indistinguishable from a complete one, and a proof of
	// absence bearing on a sum prompt stops being one. When non-null, it points at
	// a counter owned by the calling Search.
	uint64_t* subsets_capped = nullptr;
	// Explores only one representative free zone per zone type. False by default:
	// link arrows and columns can change everything.
	bool canonical_zones = false;
	// Card identity on selection prompts is UNCONDITIONAL: filling `Choice::card`
	// is not a preference, it is reading information the message already carries.
	// What was ever debatable is not the identity but the HINT BIAS applied to it,
	// and that is what `IsSubsetPrompt` in search.cpp now guards.
	// Identity on yes/no prompts is unconditional for the same reason: a prompt
	// that carries its code and its description in the message has no reason to
	// throw them away. Without it, EVERY "yes" of the game shared a SINGLE policy
	// weight.
	// OFFER PROBE: the instrument that DECOMPOSES the arity law.
	//
	// The frequency of a summon collapses with its number of materials, and the
	// raw figure cannot say WHERE. Two opposite failures produce the same zero:
	//   - the Fusion is NEVER OFFERED, because the core only lists it when its
	//     materials are payable AT THAT INSTANT: the failure is in the STATE,
	//     hence in `h` or in the decomposition;
	//   - it is OFFERED and never taken: the failure is in the SAMPLING (subset
	//     truncation, policy weights).
	// `watch`: canonical codes under watch (at most 4, same list as
	// SearchConfig::probe_watch). `watch_offered`: one bit per code, SET as soon as
	// the code appears in a prompt's pool. STRICTLY observational: no choice added,
	// removed or reweighted. Null costs one pointer test.
	const std::vector<uint32_t>* watch = nullptr;
	uint64_t* watch_offered = nullptr;
	// RESOLVED ASSIGNMENT (Delarue et al., arXiv:2010.12001: action selection posed
	// as OPTIMISATION instead of being enumerated then sampled).
	// THE DEFECT IT FIXES. `max_subsets` caps the enumeration: at 64 subsets out of
	// C(n,k) possible ones, the subset that serves the plan is not ARBITRARILY
	// rare, it is ABSENT, because the truncation is lexicographic, not random. No
	// policy weight can make up for a choice that is never emitted.
	// WHAT IT IS. CANONICAL codes that serve a requirement of a recipe of a missing
	// target card, computed by the Search from the recipe graph; the enumerator
	// stays ignorant of the goal, it only receives a list. When non-empty, SUBSET
	// prompts ALSO emit the two EXTREME subsets by that count (the most useful and
	// the least useful), deduplicated against what has already been emitted.
	// BOTH EXTREMES AND NOT ONE, and this is the honest part: a selection prompt
	// does not say whether it is asking for materials (we want useful ones) or for
	// cards to discard (we want useless ones). So we do not DECIDE the sign: we
	// guarantee that both informative subsets are in the pool, and the policy
	// learns which one, through plan_key. The mechanism never removes anything
	// from the space.
	const std::vector<uint32_t>* assign_useful = nullptr;
	// Build the human-readable labels. Hot paths (rollouts, LDS) do not need them:
	// each label is one string allocation per choice.
	bool labels = false;
	// Alias table, to conflate artwork variants. Without it, two copies of the same
	// Quetzalcoatl (29053656 and 29053657) produce different edges: a line recorded
	// on one deck would not recognise itself in the other, and deduplication would
	// let duplicates through.
	const CardDB* db = nullptr;
	// Forbidden activations: canonical code -> mask of the LOCATION_ values the
	// card must not be activated from. The choice is simply never emitted; the
	// constraint removes the branch from the space instead of rejecting it after
	// the fact. Covers idle commands only: that is where the "superfluous" ignition
	// effects live (pay 2000 LP...) that one wants to ban, and forbidding a chain
	// window would change what the rules allow.
	const std::map<uint32_t, uint32_t>* no_activate = nullptr;
	// Cards the player never CHAINS (canonical codes, --no-chain). The guards
	// (Zalen, Crystal Wing) exist to answer Nibiru, a HYPOTHETICAL threat held by
	// the guard; playing solo, every chain window answers our OWN actions, and
	// chaining their negation onto our own activations only multiplies useless
	// branches. "Do not chain" is still always offered, FORCED triggers (mandatory
	// chains) are exempt, and so are idle commands.
	const std::vector<uint32_t>* no_chain = nullptr;
	// MAIN PHASE 1 ONLY (--mp1-only): entering the Battle Phase is removed from
	// the enumeration (Main 2 only exists after the BP, so removing it too is just
	// belt and braces for prefixes recorded before the constraint). "-> End Phase"
	// always remains: the turn has to close, and removing the last legal answer
	// would corrupt the space rather than constrain it.
	bool mp1_only = false;
};

// Fills `out` (reused, see ChoiceList) with the legal answers. Empty when the
// message is not a prompt, or when its answer space cannot be safely enumerated
// (ANNOUNCE_*).
void EnumerateInto(uint8_t message, const uint8_t* data, uint32_t len,
				   const EnumOptions& opt, ChoiceList& out);

// Allocating variant, for cold paths (plan recording, coverage check): labels
// are always built.
std::vector<Choice> Enumerate(uint8_t message, const uint8_t* data, uint32_t len,
							  const EnumOptions& opt);

// Acceptable default answer for non-enumerable prompts, so the game can carry
// on instead of going into MSG_RETRY.
bool DefaultResponse(uint8_t message, const uint8_t* data, uint32_t len,
					 std::vector<uint8_t>& out);

// A RECORDED answer bypasses the enumerator, hence its forbidden-activation
// filter: this test catches it (repair mode, where the reference's answer is a
// zero-cost candidate without being enumerated).
//
// THREE-STATE. The old version returned `false` = ALLOWED on all of its failure
// paths, although it decodes a hard-coded ocgcore message layout: a format
// drift made --no-activate and --no-chain stop filtering, SILENTLY, while still
// being believed.
enum class Verdict { Allowed, Forbidden, Undecodable };
Verdict ResponseVerdict(uint8_t message, const uint8_t* data, uint32_t len,
						const std::vector<uint8_t>& response,
						const EnumOptions& opt);

// Conservative wrapper: `Undecodable` counts as FORBIDDEN, so the branch is
// removed rather than admitted unchecked, and the case is counted.
bool ResponseForbidden(uint8_t message, const uint8_t* data, uint32_t len,
					   const std::vector<uint8_t>& response,
					   const EnumOptions& opt);

// Number of answers the decoder could not read. NON-ZERO means the core's
// message layout has drifted: the filters no longer mean anything and the run
// must be thrown away. Printed in the summary.
uint64_t UndecodableResponses();

// --- WHAT A RECORDED ANSWER ACTIVATES (operator harness) ---------------------
//
// The validation harness confronts the OPERATOR TABLE extracted from the
// scripts with a RESOLVED plan: does every activation of the line correspond to
// a declared operator, and under which zone? The (code, description) pair is
// the identity the prompts carry; here we read it back from the RECORDED
// answer, which `Enumerate` does not do (it enumerates what is possible, not
// what was played).
//
// The zone was the missing piece: `SetRange` declares where the effect can be
// used, and the message carries the PHYSICAL zone the card activates from.
// Confronting the two is the only precondition a static analysis can check
// without evaluating a closure.
struct ActivationRead {
	uint32_t code = 0;
	uint64_t desc = 0;
	uint8_t location = 0;    // 0 = the message carries no zone
	uint32_t sequence = 0;
	bool has_zone = false;
};

// Returns false when the answer activates nothing (summon, phase change, "do
// not chain", selection prompt) OR when the message does not decode. The two
// cases are told apart by `UndecodableResponses()`, which counts the second.
bool DecodeActivation(uint8_t message, const uint8_t* data, uint32_t len,
					  const std::vector<uint8_t>& response,
					  const EnumOptions& opt, ActivationRead& out);

} // namespace solver
