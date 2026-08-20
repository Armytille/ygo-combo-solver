#include "enumerate.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "arena.h"   // profile probe (--profile)
#include "ocgapi_constants.h"

namespace solver {
namespace {

class Reader {
public:
	Reader(const uint8_t* d, uint32_t n) : data(d), len(n) {}
	template<typename T> T Get() {
		T v{};
		if(pos + sizeof(T) > len) { ok = false; return v; }
		std::memcpy(&v, data + pos, sizeof(T));
		pos += sizeof(T);
		return v;
	}
	void Skip(uint32_t n) { if(pos + n > len) ok = false; else pos += n; }
	bool Ok() const { return ok; }
private:
	const uint8_t* data;
	uint32_t len, pos{ 0 };
	bool ok{ true };
};

// Write IN PLACE into the response of a reused Choice: resize() on a vector
// that already has the capacity does not allocate.
void PutInt32(std::vector<uint8_t>& r, int32_t v) {
	r.resize(4);
	std::memcpy(r.data(), &v, 4);
}

// [int32 type=2][uint32 count][uint8 index...]  (playerop.cpp:258)
void PutCardIndex(std::vector<uint8_t>& r, const uint8_t* idx, size_t n) {
	r.resize(8 + n);
	int32_t type = 2;
	uint32_t count = static_cast<uint32_t>(n);
	std::memcpy(r.data(), &type, 4);
	std::memcpy(r.data() + 4, &count, 4);
	if(n)
		std::memcpy(r.data() + 8, idx, n);
}

uint64_t Mix(uint64_t h, uint64_t v) {
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}

// THE CODE A DESCRIPTION CARRIES.
//
// `MSG_SELECT_YESNO` carries no code: the description carries it instead. The
// convention is NOT `desc >> 4` (from "aux.Stringid(id, n) = id * 16 + n"): the
// contemporary core does `(n & 0xfffff) | code << 20` in `utility.lua:834`, and
// the descriptions recorded on a resolved plan are worth `code << 20`.
//
// Getting it wrong is invisible without an instrument: `Find()` fails on the
// wrong code, the code stays at ZERO, and the prompt keeps an identity (the
// edge carries `desc`) while losing its CARD, so the hint bias and the offer
// probes go blind on the pivot of the combo, silently.
//
// BOTH FORMATS ARE TRIED, the wide one first, and a candidate is kept only when
// the DATABASE knows it: a wrong shift would otherwise manufacture a plausible
// code, i.e. a false identity credited to another card.
uint32_t CodeFromDesc(uint64_t desc, const CardDB* db) {
	if(!db || !desc)
		return 0;
	const uint32_t wide = static_cast<uint32_t>(desc >> 20);
	if(wide && db->Find(wide))
		return wide;
	// Old 32-bit format: only meaningful when the description fits in it.
	if(desc < (1ull << 32)) {
		const uint32_t narrow = static_cast<uint32_t>(desc >> 4);
		if(narrow && db->Find(narrow))
			return narrow;
	}
	return 0;
}

uint64_t EdgeOf(uint8_t message, std::initializer_list<uint64_t> parts) {
	uint64_t h = message * 0x100000001b3ull;
	for(uint64_t p : parts)
		h = Mix(h, p);
	return h;
}

// Label built only on request: each label is one string allocation per choice,
// and hot paths read none of them.
void SetLabel(Choice& c, const EnumOptions& opt, const char* prefix,
			  uint64_t num, bool with_num = true) {
	if(!opt.labels)
		return;
	c.label = prefix;
	if(with_num)
		c.label += std::to_string(num);
}

// Subsets of `n` elements of size lo..hi, capped, delivered to the callback one
// at a time, with no intermediate storage.
//
// SIZES ARE WALKED ALTERNATING FROM BOTH ENDS: lo, hi, lo+1, hi-1... A minimal
// selection (saving resources) and a maximal one are the two that carry most of
// the information.
//
// Going from lo upwards and stopping at the cap is a trap: with cap = 24 and 24
// candidates, the 24 emissions are the 24 SINGLETONS, never a pair. On
// MSG_SELECT_SUM (level sums, tributes) a single-card selection almost never
// satisfies the constraint, so the prompt goes sterile without saying so and
// any proof of absence bearing on a Synchro or tribute summon is worthless.
//
// `capped`, when non-null, gets +1 whenever the cap actually truncated: a
// truncated enumeration must be distinguishable from a complete one.
template<typename F>
void ForEachSubset(uint32_t n, uint32_t lo, uint32_t hi, uint32_t cap, F&& f,
				   uint64_t* capped = nullptr, bool ascending = false) {
	static thread_local std::vector<uint8_t> cur;
	hi = (std::min)(hi, n);
	if(lo > hi)
		return;
	uint32_t emitted = 0;
	uint32_t klo = lo, khi = hi;
	bool from_low = true;
	while(klo <= khi) {
		if(emitted >= cap) {
			if(capped)
				++*capped;
			return;
		}
		uint32_t k;
		if(ascending || from_low || klo == khi) {
			k = klo;
			++klo;
		} else {
			k = khi;
			--khi;   // khi > klo >= 0 here: no overflow
		}
		from_low = !from_low;
		if(k == 0) {
			f(nullptr, 0u);
			++emitted;
			continue;
		}
		cur.resize(k);
		for(uint32_t i = 0; i < k; ++i)
			cur[i] = static_cast<uint8_t>(i);
		for(;;) {
			f(cur.data(), k);
			if(++emitted >= cap) {
				// The cap bites: either combinations of this size, or whole
				// sizes, remain unvisited.
				if(capped)
					++*capped;
				return;
			}
			// next combination in lexicographic order
			int i = static_cast<int>(k) - 1;
			while(i >= 0 && cur[i] == n - k + i)
				--i;
			if(i < 0)
				break;
			++cur[i];
			for(uint32_t j = i + 1; j < k; ++j)
				cur[j] = static_cast<uint8_t>(cur[j - 1] + 1);
		}
	}
}

constexpr uint32_t kLocInfo = 1 + 1 + 4 + 4;

// RESOLVED ASSIGNMENT: emits the EXTREME subsets by `opt.assign_useful`, which
// ForEachSubset's truncation hides as soon as C(n,k) exceeds `max_subsets`.
//
// TWO SIZES (minimal and maximal) x TWO EXTREMES (most useful and least useful)
// = at most four extra choices, deduplicated against what has already been
// emitted. So we never add one that already existed: the mechanism fills a HOLE
// in the enumeration, it does not reweight what the enumeration covered.
//
// `pool` indexes `codes` and already carries the caller's per-code dedup.
void EmitAssignExtremes(uint8_t message, const EnumOptions& opt,
						const std::vector<uint8_t>& pool,
						const std::vector<uint32_t>& codes, uint32_t lo,
						uint32_t hi, ChoiceList& out) {
	if(!opt.assign_useful || opt.assign_useful->empty() || pool.empty())
		return;
	const auto& useful = *opt.assign_useful;
	auto is_useful = [&useful](uint32_t code) {
		return std::find(useful.begin(), useful.end(), code) != useful.end();
	};
	// Positions in `pool` sorted by decreasing usefulness. STABLE sort: two runs
	// with the same seed must emit exactly the same subsets, otherwise they do
	// not explore the same action space.
	static thread_local std::vector<uint8_t> rank, sel;
	rank.resize(pool.size());
	for(size_t i = 0; i < pool.size(); ++i)
		rank[i] = static_cast<uint8_t>(i);
	std::stable_sort(rank.begin(), rank.end(),
					 [&](uint8_t a, uint8_t b) {
						 return is_useful(codes[pool[a]]) >
								is_useful(codes[pool[b]]);
					 });
	const uint32_t n = static_cast<uint32_t>(pool.size());
	const uint32_t khi = (std::min)(hi, n);
	const uint32_t klo = (std::max)(lo, 1u);
	if(klo > khi)
		return;
	const uint32_t ks[2] = { klo, khi };
	const uint32_t nk = (ks[0] == ks[1]) ? 1u : 2u;
	for(uint32_t ki = 0; ki < nk; ++ki) {
		const uint32_t k = ks[ki];
		for(int end = 0; end < 2; ++end) {
			sel.clear();
			for(uint32_t j = 0; j < k; ++j)
				sel.push_back(pool[rank[end ? n - 1 - j : j]]);
			// ASCENDING SORT is mandatory: it is the order in which
			// ForEachSubset emits its members, and the edge is an ORDERED Mix.
			// Without this sort the same choice would carry two different edges
			// and the dedup below would let a duplicate through.
			std::sort(sel.begin(), sel.end());
			uint64_t h = 0;
			for(uint8_t ix : sel)
				h = Mix(h, codes[ix]);
			const uint64_t edge = EdgeOf(message, { h, k });
			bool dup = false;
			for(const Choice& c : out)
				if(c.edge == edge) { dup = true; break; }
			if(dup)
				continue;
			Choice& c = out.Emit();
			PutCardIndex(c.response, sel.data(), sel.size());
			c.edge = edge;
			c.card = codes[sel[0]];
			if(opt.labels)
				c.label = std::string(end ? "assign- " : "assign+ ") +
						  std::to_string(k);
		}
	}
}

void EnumerateRaw(uint8_t message, const uint8_t* data, uint32_t len,
				  const EnumOptions& opt, ChoiceList& out) {
	Reader r(data, len);
	// The codes read from the prompt are the PRINTED codes: two artworks of the
	// same card do not carry the same number there. So every edge is built on the
	// canonical code, otherwise it designates a printing rather than a card, which
	// is useless for transposition and wrong for dedup.
	// OFFER PROBE grafted onto the MANDATORY choke point: every code read from a
	// prompt, whatever the message, goes through `canon`. Marking per message would
	// have missed a prompt silently, and "never offered" is exactly the conclusion
	// an incomplete probe would manufacture.
	// Marking comes before the filters (dedup, --no-activate, --no-chain), on
	// purpose: the question is what THE GAME offers, not what we keep of it.
	auto canon = [&opt](uint32_t code) {
		const uint32_t c = opt.db ? opt.db->Canonical(code) : code;
		if(opt.watch_offered && opt.watch) {
			const size_t n = (std::min)(opt.watch->size(), size_t(4));
			for(size_t i = 0; i < n; ++i)
				if((*opt.watch)[i] == c)
					*opt.watch_offered |= 1ull << i;
		}
		return c;
	};
	// Dedup without std::set: the lists are short (a few dozen) and a linear scan
	// through a reused buffer beats a nest of allocations.
	static thread_local std::vector<uint32_t> seen_codes;
	static thread_local std::vector<std::pair<uint32_t, uint64_t>> seen_pairs;
	static thread_local std::vector<uint32_t> codes;
	// (code, location) for deduplicating `SELECT_UNSELECT_CARD`: the code alone is
	// not enough, see the coverage fix below.
	static thread_local std::vector<uint64_t> seen_keys;
	static thread_local std::vector<uint64_t> loc_keys;

	switch(message) {
	case MSG_SELECT_IDLECMD: {
		r.Get<uint8_t>();   // playerid
		// t: 0 summon, 1 special summon, 2 position change, 3 set monster, 4 set
		// spell/trap, 5 activation.
		// The answer is t | (s << 16)  (playerop.cpp:141)
		static const char* kNames[5] = { "invoquer ", "inv.speciale ",
										 "reposition ", "poser-mon ", "poser-st " };
		const uint32_t strides[5] = { 10, 10, 7, 10, 10 };
		for(uint32_t g = 0; g < 5; ++g) {
			uint32_t n = r.Get<uint32_t>();
			seen_codes.clear();
			for(uint32_t i = 0; i < n && r.Ok(); ++i) {
				uint32_t code = canon(r.Get<uint32_t>());
				r.Skip(strides[g] - 4);
				if(opt.dedup_by_code) {
					if(std::find(seen_codes.begin(), seen_codes.end(), code) !=
					   seen_codes.end())
						continue;
					seen_codes.push_back(code);
				}
				Choice& c = out.Emit();
				PutInt32(c.response, static_cast<int32_t>(g | (i << 16)));
				c.edge = EdgeOf(message, { g, code, opt.dedup_by_code ? 0u : i });
				c.card = code;
				SetLabel(c, opt, kNames[g], code);
			}
		}
		uint32_t n_act = r.Get<uint32_t>();
		seen_pairs.clear();
		for(uint32_t i = 0; i < n_act && r.Ok(); ++i) {
			uint32_t code = canon(r.Get<uint32_t>());
			r.Skip(1);                       // controller
			uint8_t loc = r.Get<uint8_t>();  // zone the card activates from
			r.Skip(4);                       // sequence
			uint64_t desc = r.Get<uint64_t>();
			r.Skip(1);
			// Activation forbidden from this zone: the choice does not exist.
			if(opt.no_activate) {
				auto it = opt.no_activate->find(code);
				if(it != opt.no_activate->end() && (it->second & loc))
					continue;
			}
			if(opt.dedup_by_code) {
				std::pair<uint32_t, uint64_t> key{ code, desc };
				if(std::find(seen_pairs.begin(), seen_pairs.end(), key) !=
				   seen_pairs.end())
					continue;
				seen_pairs.push_back(key);
			}
			Choice& c = out.Emit();
			PutInt32(c.response, static_cast<int32_t>(5u | (i << 16)));
			c.edge = EdgeOf(message, { 5, code, desc });
			c.card = code;
			SetLabel(c, opt, "activer ", code);
		}
		uint8_t to_bp = r.Get<uint8_t>();
		uint8_t to_ep = r.Get<uint8_t>();
		r.Get<uint8_t>();   // shuffling the hand: no effect on the board
		if(!r.Ok()) {
			out.Clear();
			return;
		}
		// Same safeguard as at the battle prompt: an idle prompt with no playable
		// command keeps its phase exits, otherwise the flag would manufacture a dead
		// end where the game offered a way out.
		{
			// --mp1-only: the BP disappears as long as the EP exit exists (never
			// remove the last legal answer of a prompt).
			if(to_bp && !(opt.mp1_only && to_ep)) {
				Choice& c = out.Emit();
				PutInt32(c.response, 6);
				c.edge = EdgeOf(message, { 6 });
				c.phase = true;
				SetLabel(c, opt, "-> Battle Phase", 0, false);
			}
			if(to_ep) {
				Choice& c = out.Emit();
				PutInt32(c.response, 7);
				c.edge = EdgeOf(message, { 7 });
				c.phase = true;
				SetLabel(c, opt, "-> End Phase", 0, false);
			}
		}
		return;
	}

	case MSG_SELECT_BATTLECMD: {
		r.Get<uint8_t>();
		uint32_t n_act = r.Get<uint32_t>();
		for(uint32_t i = 0; i < n_act && r.Ok(); ++i) {
			uint32_t code = canon(r.Get<uint32_t>());
			r.Skip(1 + 1 + 4);
			uint64_t desc = r.Get<uint64_t>();
			r.Skip(1);
			Choice& c = out.Emit();
			PutInt32(c.response, static_cast<int32_t>(0u | (i << 16)));
			c.edge = EdgeOf(message, { 0, code, desc });
			c.card = code;
			SetLabel(c, opt, "activer ", code);
		}
		uint32_t n_atk = r.Get<uint32_t>();
		for(uint32_t i = 0; i < n_atk && r.Ok(); ++i) {
			uint32_t code = canon(r.Get<uint32_t>());
			// DECODING FIX, checked against `playerop.cpp:37`: in the ATTACKABLE
			// list, `sequence` is a uint8 and not a uint32, so an entry is
			//   code u32 | controller u8 | location u8 | sequence u8 | direct u8
			// that is EIGHT bytes, where this code used to skip ELEVEN.
			//
			// WHAT THAT COST: three bytes of drift per attackable monster, so
			// `r.Ok()` falls as soon as there is ONE, which is the case of any
			// built board. `out.Clear()` follows, the enumeration comes out
			// EMPTY, and since `DefaultResponse` does not cover
			// MSG_SELECT_BATTLECMD, the branch DIES. In other words, EVERY LINE
			// ENTERING THE BATTLE PHASE was doomed, so every combo going through
			// Main 2 was out of reach, silently, since the death read like an
			// ordinary dead end.
			//
			// The count was RIGHT in the ACTIVABLE list just above (sequence is a
			// uint32 there): the difference between the two lists of the same
			// message is what kept the defect unnoticed.
			r.Skip(1 + 1 + 1 + 1);
			Choice& c = out.Emit();
			PutInt32(c.response, static_cast<int32_t>(1u | (i << 16)));
			c.edge = EdgeOf(message, { 1, code });
			c.card = code;
			SetLabel(c, opt, "attaquer avec ", code);
		}
		uint8_t to_m2 = r.Get<uint8_t>();
		uint8_t to_ep = r.Get<uint8_t>();
		if(!r.Ok()) {
			out.Clear();
			return;
		}
		// PHASE CHANGE AT THE BATTLE PROMPT. `allow_phase_change` used to be
		// read at the IDLE prompt only; here the two phase exits were emitted
		// UNCONDITIONALLY, so the flag closed only half the door. Safeguard: when
		// the prompt offers NOTHING else, they stay emitted. Removing the last
		// legal answer would turn a prompt into a dead end, which is not pruning
		// but corruption of the space.
		if(to_m2 && !(opt.mp1_only && to_ep)) {
			Choice& c = out.Emit();
			PutInt32(c.response, 2);
			c.edge = EdgeOf(message, { 2 });
			c.phase = true;
			SetLabel(c, opt, "-> Main 2", 0, false);
		}
		if(to_ep) {
			Choice& c = out.Emit();
			PutInt32(c.response, 3);
			c.edge = EdgeOf(message, { 3 });
			c.phase = true;
			SetLabel(c, opt, "-> End Phase", 0, false);
		}
		return;
	}

	case MSG_SELECT_EFFECTYN:
	case MSG_SELECT_YESNO: {
		// IDENTITY OF THE YES/NO PROMPT.
		//
		// THE DEFECT. The edge was `EdgeOf(message, {1})`: EVERY "yes" of the
		// whole game shared a SINGLE policy weight, and every "no" another. NRPA
		// could only learn a global propensity to say yes.
		//
		// WHY THIS PROMPT MATTERS. On benchmark A, the decision that unlocks the
		// materials in the GRAVEYARD is a `Duel.SelectYesNo`: effect e2 of
		// `Lunalight Masquerade` offers to discard a card, and that discard
		// registers EFFECT_EXTRA_FUSION_MATERIAL until the End Phase. Without it
		// the following Fusions have no access to the graveyard, where the named
		// material lives. The resolved plan counts only 6 EFFECTYN and 2 YESNO
		// out of 284 decisions: two weights for eight decisions, one of them the
		// pivot.
		//
		// WHAT GIVES THE IDENTITY. `MSG_SELECT_EFFECTYN` carries the code and the
		// description; `MSG_SELECT_YESNO` carries ONLY the description. But the
		// `aux.Stringid` convention is universal in the scripts: the description
		// CARRIES the card's code. It is the only way to attribute a YESNO.
		uint32_t code = 0;
		uint64_t desc = 0;
		{
			Reader r2(data, len);
			r2.Get<uint8_t>();   // playerid
			if(message == MSG_SELECT_EFFECTYN) {
				code = canon(r2.Get<uint32_t>());
				r2.Skip(kLocInfo);
			}
			desc = r2.Get<uint64_t>();
			if(!r2.Ok()) {
				code = 0;
				desc = 0;
			} else if(!code && desc) {
				code = canon(CodeFromDesc(desc, opt.db));
			}
		}
		Choice& y = out.Emit();
		PutInt32(y.response, 1);
		y.edge = EdgeOf(message, { code, desc, 1 });
		y.card = code;
		SetLabel(y, opt, "oui", 0, false);
		Choice& n = out.Emit();
		PutInt32(n.response, 0);
		n.edge = EdgeOf(message, { code, desc, 0 });
		n.card = code;
		SetLabel(n, opt, "non", 0, false);
		return;
	}

	case MSG_SELECT_OPTION: {
		r.Get<uint8_t>();
		uint8_t n = r.Get<uint8_t>();
		for(uint8_t i = 0; i < n && r.Ok(); ++i) {
			uint64_t desc = r.Get<uint64_t>();
			Choice& c = out.Emit();
			PutInt32(c.response, i);
			c.edge = EdgeOf(message, { desc });
			SetLabel(c, opt, "option ", i);
		}
		return;
	}

	case MSG_SELECT_CHAIN: {
		r.Get<uint8_t>();
		r.Get<uint8_t>();                 // spe_count
		uint8_t forced = r.Get<uint8_t>();
		r.Get<uint32_t>(); r.Get<uint32_t>();
		uint32_t n = r.Get<uint32_t>();
		seen_pairs.clear();
		for(uint32_t i = 0; i < n && r.Ok(); ++i) {
			uint32_t code = canon(r.Get<uint32_t>());
			r.Skip(kLocInfo);
			uint64_t desc = r.Get<uint64_t>();
			r.Skip(1);
			// --no-chain: this card never chains (its FORCED triggers do not
			// come through here, forced is exempt below).
			// The option disappears, "do not chain" remains.
			if(!forced && opt.no_chain &&
			   std::find(opt.no_chain->begin(), opt.no_chain->end(), code) !=
				   opt.no_chain->end())
				continue;
			if(opt.dedup_by_code) {
				std::pair<uint32_t, uint64_t> key{ code, desc };
				if(std::find(seen_pairs.begin(), seen_pairs.end(), key) !=
				   seen_pairs.end())
					continue;
				seen_pairs.push_back(key);
			}
			Choice& c = out.Emit();
			PutInt32(c.response, static_cast<int32_t>(i));
			c.edge = EdgeOf(message, { code, desc });
			// The card engaged: without it the QUICK effects (Omega's rip
			// activates in a chain window) escaped the hint bias, so summons
			// were biased and chained activations never were.
			c.card = code;
			// The effect engaged: what gives --no-self-negate its precision.
			c.desc = desc;
			SetLabel(c, opt, "chainer ", code);
		}
		if(!r.Ok()) {
			out.Clear();
			return;
		}
		if(!forced) {
			Choice& c = out.Emit();
			PutInt32(c.response, -1);
			c.edge = EdgeOf(message, { uint64_t(-1) });
			SetLabel(c, opt, "ne pas chainer", 0, false);
		}
		return;
	}

	case MSG_SELECT_CARD:
	case MSG_SELECT_TRIBUTE: {
		r.Get<uint8_t>();
		uint8_t cancelable = r.Get<uint8_t>();
		uint32_t lo = r.Get<uint32_t>();
		uint32_t hi = r.Get<uint32_t>();
		uint32_t n = r.Get<uint32_t>();
		codes.clear();
		const uint32_t stride = (message == MSG_SELECT_CARD) ? kLocInfo : 7;
		for(uint32_t i = 0; i < n && r.Ok(); ++i) {
			codes.push_back(canon(r.Get<uint32_t>()));
			r.Skip(stride);
		}
		if(!r.Ok()) {
			out.Clear();
			return;
		}
		// Cards with the same code are interchangeable: only one representative
		// per code is kept before forming the subsets.
		static thread_local std::vector<uint8_t> pool;
		pool.clear();
		seen_codes.clear();
		for(uint32_t i = 0; i < codes.size(); ++i) {
			if(opt.dedup_by_code) {
				if(std::find(seen_codes.begin(), seen_codes.end(), codes[i]) !=
				   seen_codes.end())
					continue;
				seen_codes.push_back(codes[i]);
			}
			pool.push_back(static_cast<uint8_t>(i));
		}

		static thread_local std::vector<uint8_t> idx;
		ForEachSubset(static_cast<uint32_t>(pool.size()), lo, hi, opt.max_subsets,
					  [&](const uint8_t* s, uint32_t k) {
						  idx.clear();
						  uint64_t h = 0;
						  for(uint32_t j = 0; j < k; ++j) {
							  idx.push_back(pool[s[j]]);
							  h = Mix(h, codes[pool[s[j]]]);
						  }
						  Choice& c = out.Emit();
						  PutCardIndex(c.response, idx.data(), idx.size());
						  c.edge = EdgeOf(message, { h, k });
						  // The identity of the choice, so the target-derived bias can
						  // apply to it: without it, "which Fusion to summon" is
						  // invisible to sampling (see EnumOptions).
						  if(k)
							  c.card = codes[pool[s[0]]];
						  if(opt.labels)
							  c.label = "choisir " + std::to_string(k) +
										" carte(s)";
					  },
					  opt.subsets_capped, false);
		EmitAssignExtremes(message, opt, pool, codes, lo, hi, out);
		if(cancelable) {
			Choice& c = out.Emit();
			PutInt32(c.response, -1);
			c.edge = EdgeOf(message, { uint64_t(-1) });
			SetLabel(c, opt, "annuler", 0, false);
		}
		return;
	}

	case MSG_SELECT_UNSELECT_CARD: {
		r.Get<uint8_t>();
		uint8_t finishable = r.Get<uint8_t>();
		uint8_t cancelable = r.Get<uint8_t>();
		r.Get<uint32_t>(); r.Get<uint32_t>();
		uint32_t n = r.Get<uint32_t>();
		// THE `loc_info` IS READ, NOT THROWN AWAY, and this is a coverage fix, not a
		// refinement.
		//
		// THE MEASUREMENT THAT FORCES IT. On the only known line that reaches
		// benchmark A's goal, the reference picks index 3 of a
		// `SELECT_UNSELECT_CARD` and the enumerator only offered 0, 1, 2:
		// `dedup_by_code` had folded index 3 onto a CODE duplicate. Result:
		// 251/252 moves recovered, and ONE missing move makes the line
		// unreachable at any budget.
		//
		// Two cards with the same code are NOT interchangeable on this prompt: the
		// list spans zones, and two copies differ there by location, sequence and
		// position. Deduplicating on the code alone therefore deletes REAL moves.
		// We now deduplicate on the (canonical code, location) pair; the byte was
		// already in the message, it was simply being skipped.
		codes.clear();
		loc_keys.clear();
		for(uint32_t i = 0; i < n && r.Ok(); ++i) {
			codes.push_back(canon(r.Get<uint32_t>()));
			const uint8_t con = r.Get<uint8_t>();
			const uint8_t loc = r.Get<uint8_t>();
			const uint32_t seq = r.Get<uint32_t>();
			r.Get<uint32_t>();   // position
			loc_keys.push_back((uint64_t(con) << 40) | (uint64_t(loc) << 32) |
							   seq);
		}
		uint32_t n_un = r.Get<uint32_t>();
		if(!r.Ok()) {
			out.Clear();
			return;
		}
		// [int32 1][int32 index]  (playerop.cpp:439)
		auto make = [&](uint32_t index, uint64_t key, const char* what) {
			Choice& c = out.Emit();
			c.response.resize(8);
			int32_t one = 1, idx32 = static_cast<int32_t>(index);
			std::memcpy(c.response.data(), &one, 4);
			std::memcpy(c.response.data() + 4, &idx32, 4);
			c.edge = EdgeOf(message, { key });
			SetLabel(c, opt, what, 0, false);
		};
		seen_keys.clear();
		for(uint32_t i = 0; i < codes.size(); ++i) {
			if(opt.dedup_by_code) {
				// Key = (code, location). Two copies of the same code in the SAME
				// zone stay folded (they are interchangeable); two copies in
				// different zones do not.
				const uint64_t key =
					(uint64_t(codes[i]) << 24) ^ loc_keys[i];
				if(std::find(seen_keys.begin(), seen_keys.end(), key) !=
				   seen_keys.end())
					continue;
				seen_keys.push_back(key);
			}
			make(i, codes[i], "selectionner");
		}
		for(uint32_t i = 0; i < n_un; ++i)
			make(static_cast<uint32_t>(codes.size()) + i, 0x8000000ull + i,
				 "deselectionner");
		if(finishable || cancelable) {
			Choice& c = out.Emit();
			PutInt32(c.response, -1);
			c.edge = EdgeOf(message, { uint64_t(-1) });
			SetLabel(c, opt, "terminer", 0, false);
		}
		return;
	}

	case MSG_SELECT_PLACE:
	case MSG_SELECT_DISFIELD: {
		r.Get<uint8_t>();   // player
		uint8_t count = r.Get<uint8_t>();
		uint32_t flag = r.Get<uint32_t>();
		if(!r.Ok() || count == 0)
			return;
		// A bit set means the zone is forbidden (playerop.cpp:590). At most 30
		// slots: a stack array is enough.
		struct Slot { uint8_t owner, loc, seq; };
		Slot free_slots[30];
		uint32_t n_free = 0;
		for(uint8_t owner = 0; owner < 2; ++owner) {
			for(uint8_t seq = 0; seq < 7; ++seq)
				if(!(flag & (1u << (seq + owner * 16))))
					free_slots[n_free++] = { owner, LOCATION_MZONE, seq };
			for(uint8_t seq = 0; seq < 8; ++seq)
				if(!(flag & (1u << (seq + 8 + owner * 16))))
					free_slots[n_free++] = { owner, LOCATION_SZONE, seq };
		}
		if(opt.canonical_zones) {
			// One representative zone per (owner, zone type).
			//
			// MEASURED HAZARD, and it is enough to condemn this flag as it stands:
			// PENDULUM ZONES are PARTICULAR sequences of LOCATION_SZONE, and
			// canonicalisation conflates them with an ordinary spell set, so it
			// REMOVES the possibility of setting a scale. On benchmark A,
			// `Lunalight Wolf` only Fusion Summons from the Pendulum Zone
			// (`e2:SetRange(LOCATION_PZONE)`): this flag silently closes TWO of the
			// combo's three doors.
			// The correct rule is not "one zone per type" but "one zone per
			// EQUIVALENCE CLASS THE RULES RESPECT": the Pendulum Zone, and a zone
			// pointed at by a Link, are classes of their own.
			// That is why it stays off, and why judging it without that fix would
			// return a falsely negative verdict.
			uint32_t kept = 0;
			for(uint32_t i = 0; i < n_free; ++i) {
				bool dup = false;
				for(uint32_t j = 0; j < kept; ++j)
					if(free_slots[j].owner == free_slots[i].owner &&
					   free_slots[j].loc == free_slots[i].loc) {
						dup = true;
						break;
					}
				if(!dup)
					free_slots[kept++] = free_slots[i];
			}
			n_free = kept;
		}
		// In the vast majority of cases only one card is placed at a time.
		if(count == 1) {
			for(uint32_t i = 0; i < n_free; ++i) {
				const Slot& s = free_slots[i];
				Choice& c = out.Emit();
				c.response.resize(3);
				c.response[0] = s.owner;
				c.response[1] = s.loc;
				c.response[2] = s.seq;
				c.edge = EdgeOf(message, { s.owner, s.loc, s.seq });
				// Plan identity ignores the column: two placements in two
				// free columns carry out the same intent.
				c.plan_key = EdgeOf(message, { s.owner, s.loc });
				SetLabel(c, opt, "zone ", s.seq);
			}
		} else {
			// Multiple placement: take the first free zones.
			if(n_free >= count) {
				Choice& c = out.Emit();
				c.response.resize(size_t(count) * 3);
				for(uint8_t i = 0; i < count; ++i) {
					c.response[size_t(i) * 3 + 0] = free_slots[i].owner;
					c.response[size_t(i) * 3 + 1] = free_slots[i].loc;
					c.response[size_t(i) * 3 + 2] = free_slots[i].seq;
				}
				c.edge = EdgeOf(message, { count });
				SetLabel(c, opt, "placement multiple", 0, false);
			}
		}
		return;
	}

	case MSG_SELECT_POSITION: {
		r.Get<uint8_t>();
		// The CARD is part of the choice's identity. Without it, "ATK" and "DEF"
		// are two global moves: the NRPA policy learns ONE weight for every
		// position of every card (measured: six monsters in DEF where the target
		// wants five in ATK) and the repertoire loses the reference's per-card
		// intent.
		uint32_t code = canon(r.Get<uint32_t>());
		uint8_t pos = r.Get<uint8_t>() & 0xf;
		if(!r.Ok())
			return;
		static const struct { uint8_t bit; const char* name; } kPos[] = {
			{ POS_FACEUP_ATTACK, "ATK " }, { POS_FACEDOWN_ATTACK, "FD-ATK " },
			{ POS_FACEUP_DEFENSE, "DEF " }, { POS_FACEDOWN_DEFENSE, "FD-DEF " },
		};
		for(const auto& p : kPos)
			if(pos & p.bit) {
				Choice& c = out.Emit();
				PutInt32(c.response, p.bit);
				c.edge = EdgeOf(message, { code, p.bit });
				c.card = code;
				SetLabel(c, opt, p.name, code);
			}
		return;
	}

	case MSG_SELECT_SUM: {
		r.Get<uint8_t>();
		r.Get<uint8_t>();                 // mode
		r.Get<uint32_t>();                // acc
		r.Get<uint32_t>(); r.Get<uint32_t>();
		uint32_t n_must = r.Get<uint32_t>();
		for(uint32_t i = 0; i < n_must && r.Ok(); ++i)
			r.Skip(4 + kLocInfo + 4);
		uint32_t n = r.Get<uint32_t>();
		codes.clear();
		for(uint32_t i = 0; i < n && r.Ok(); ++i) {
			codes.push_back(canon(r.Get<uint32_t>()));
			r.Skip(kLocInfo + 4);
		}
		if(!r.Ok()) {
			out.Clear();
			return;
		}
		// The sum constraint can only be checked by the core: we offer every
		// subset, the invalid ones get rejected (MSG_RETRY) and the branch is
		// abandoned.
		ForEachSubset(n, 1, n, opt.max_subsets,
					  [&](const uint8_t* s, uint32_t k) {
						  uint64_t h = 0;
						  for(uint32_t j = 0; j < k; ++j)
							  h = Mix(h, codes[s[j]]);
						  Choice& c = out.Emit();
						  PutCardIndex(c.response, s, k);
						  c.edge = EdgeOf(message, { h, k });
						  if(opt.labels)
							  c.label = "somme " + std::to_string(k);
					  },
					  opt.subsets_capped, false);
		// The SUM prompt is the one for tributes and Synchro materials: it is
		// where benchmark B pays its arity. No dedup by code here (the
		// enumeration indexes `codes` directly), so the pool is the identity.
		// l'identite.
		if(opt.assign_useful && !opt.assign_useful->empty()) {
			static thread_local std::vector<uint8_t> ident;
			ident.resize(codes.size());
			for(size_t i = 0; i < codes.size(); ++i)
				ident[i] = static_cast<uint8_t>(i);
			EmitAssignExtremes(message, opt, ident, codes, 1, n, out);
		}
		return;
	}

	case MSG_SELECT_COUNTER:
	case MSG_SORT_CARD:
	case MSG_ANNOUNCE_RACE:
	case MSG_ANNOUNCE_ATTRIB:
	case MSG_ANNOUNCE_CARD:
	case MSG_ANNOUNCE_NUMBER: {
		// Value spaces, not lists: a naive enumeration would be huge and, above
		// all, irrelevant to board equality. We settle for the default answer.
		static thread_local std::vector<uint8_t> def;
		def.clear();
		if(DefaultResponse(message, data, len, def)) {
			Choice& c = out.Emit();
			c.response = def;
			c.edge = EdgeOf(message, { 0 });
			SetLabel(c, opt, "defaut", 0, false);
		}
		return;
	}

	default:
		return;
	}
}

} // namespace

void EnumerateInto(uint8_t message, const uint8_t* data, uint32_t len,
				   const EnumOptions& opt, ChoiceList& out) {
	prof::Scope ps(prof::kEnumerate);
	out.Clear();
	EnumerateRaw(message, data, len, opt, out);
	// By default the plan identity is the edge itself; only the prompts that
	// explicitly distinguish it (zone choice) fill it in.
	for(Choice& c : out)
		if(!c.plan_key)
			c.plan_key = c.edge;
}

std::vector<Choice> Enumerate(uint8_t message, const uint8_t* data, uint32_t len,
							  const EnumOptions& opt) {
	static thread_local ChoiceList scratch;
	EnumOptions o = opt;
	o.labels = true;   // cold callers read the labels
	EnumerateInto(message, data, len, o, scratch);
	return std::vector<Choice>(scratch.begin(), scratch.end());
}

bool DefaultResponse(uint8_t message, const uint8_t* data, uint32_t len,
					 std::vector<uint8_t>& out) {
	Reader r(data, len);
	switch(message) {
	case MSG_SORT_CARD: {
		// Identity order: deck order plays no part in the board equivalence we
		// use.
		r.Get<uint8_t>();
		uint32_t n = r.Get<uint32_t>();
		if(!r.Ok())
			return false;
		out.assign(n, 0);
		for(uint32_t i = 0; i < n; ++i)
			out[i] = static_cast<uint8_t>(i);
		return true;
	}
	case MSG_SELECT_COUNTER: {
		r.Get<uint8_t>();
		r.Get<uint16_t>();
		uint16_t count = r.Get<uint16_t>();
		uint32_t n = r.Get<uint32_t>();
		if(!r.Ok())
			return false;
		// Remove the counters from the first available cards.
		out.assign(size_t(n) * 2, 0);
		std::vector<uint16_t> avail;
		for(uint32_t i = 0; i < n && r.Ok(); ++i) {
			r.Skip(4 + 1 + 1 + 1);
			avail.push_back(r.Get<uint16_t>());
		}
		uint16_t left = count;
		for(uint32_t i = 0; i < n && left; ++i) {
			uint16_t take = (std::min)(left, i < avail.size() ? avail[i] : uint16_t(0));
			std::memcpy(out.data() + size_t(i) * 2, &take, 2);
			left = static_cast<uint16_t>(left - take);
		}
		return left == 0;
	}
	case MSG_ANNOUNCE_NUMBER: {
		r.Get<uint8_t>();
		uint8_t n = r.Get<uint8_t>();
		if(!r.Ok() || n == 0)
			return false;
		out.resize(4);
		int32_t v = 0;   // first choice offered
		std::memcpy(out.data(), &v, 4);
		return true;
	}
	case MSG_ANNOUNCE_RACE:
	case MSG_ANNOUNCE_ATTRIB: {
		// We must announce one bit among those allowed; without fine decoding, we
		// let the branch fail rather than risk an illegal answer.
		return false;
	}
	default:
		return false;
	}
}

// Three-state. All the failure paths used to return `false` = ALLOWED, on a
// hard-coded ocgcore message layout (`strides[5]`). If the pinned core moves a
// field, the reader desynchronises, `r.Ok()` falls, and --no-activate /
// --no-chain simply stop filtering: the recorded answer carrying the forbidden
// activation is readmitted at zero cost, silently. A filter that fails OPEN on
// a format drift is worse than no filter, because it goes on being believed.
//
// So now `Undecodable` is distinct from `Allowed`, and callers treat it as a
// fatal error rather than as permission.
Verdict ResponseVerdict(uint8_t message, const uint8_t* data, uint32_t len,
						const std::vector<uint8_t>& response,
						const EnumOptions& opt) {
	// --no-chain: a RECORDED answer that chains a forbidden card is caught here
	// (repair mode, where the reference's answer is a zero-cost candidate without
	// going through the enumerator).
	if(message == MSG_SELECT_CHAIN && response.size() == 4 && opt.no_chain &&
	   !opt.no_chain->empty()) {
		int32_t v = 0;
		std::memcpy(&v, response.data(), 4);
		if(v < 0)
			return Verdict::Allowed;   // "do not chain"
		Reader r(data, len);
		r.Get<uint8_t>();
		r.Get<uint8_t>();
		uint8_t forced = r.Get<uint8_t>();
		r.Get<uint32_t>();
		r.Get<uint32_t>();
		uint32_t n = r.Get<uint32_t>();
		if(!r.Ok())
			return Verdict::Undecodable;
		// A FORCED chain is not a choice: the filter does not apply to it.
		if(forced)
			return Verdict::Allowed;
		// An out-of-range index is not a read drift: it is an answer that designates
		// nothing in THIS prompt.
		if(static_cast<uint32_t>(v) >= n)
			return Verdict::Undecodable;
		for(int32_t i = 0; i < v; ++i)
			r.Skip(4 + kLocInfo + 8 + 1);
		uint32_t code = r.Get<uint32_t>();
		if(!r.Ok())
			return Verdict::Undecodable;
		if(opt.db)
			code = opt.db->Canonical(code);
		return std::find(opt.no_chain->begin(), opt.no_chain->end(), code) !=
					   opt.no_chain->end()
				   ? Verdict::Forbidden
				   : Verdict::Allowed;
	}
	if(!opt.no_activate || opt.no_activate->empty())
		return Verdict::Allowed;
	if(message != MSG_SELECT_IDLECMD || response.size() != 4)
		return Verdict::Allowed;
	int32_t v = 0;
	std::memcpy(&v, response.data(), 4);
	uint32_t t = static_cast<uint32_t>(v) & 0xffffu;
	uint32_t s = static_cast<uint32_t>(v) >> 16;
	if(t != 5)
		return Verdict::Allowed;   // only the "activate" family is covered
	// Replay the prompt decoding up to the designated entry.
	Reader r(data, len);
	r.Get<uint8_t>();
	const uint32_t strides[5] = { 10, 10, 7, 10, 10 };
	for(uint32_t g = 0; g < 5; ++g) {
		uint32_t n = r.Get<uint32_t>();
		for(uint32_t i = 0; i < n && r.Ok(); ++i)
			r.Skip(strides[g]);
	}
	uint32_t n_act = r.Get<uint32_t>();
	if(!r.Ok() || s >= n_act)
		return Verdict::Undecodable;
	for(uint32_t i = 0; i < s; ++i)
		r.Skip(4 + 1 + 1 + 4 + 8 + 1);
	uint32_t code = r.Get<uint32_t>();
	r.Skip(1);
	uint8_t loc = r.Get<uint8_t>();
	if(!r.Ok())
		return Verdict::Undecodable;
	if(opt.db)
		code = opt.db->Canonical(code);
	auto it = opt.no_activate->find(code);
	return (it != opt.no_activate->end() && (it->second & loc) != 0)
			   ? Verdict::Forbidden
			   : Verdict::Allowed;
}

// Compatibility: `Undecodable` counts as FORBIDDEN, i.e. the branch is removed
// instead of being admitted unchecked. The global counter says how many times
// that happened; non-zero, and the message layout has drifted and the filters
// no longer mean anything.
std::atomic<uint64_t> g_undecodable{ 0 };

bool ResponseForbidden(uint8_t message, const uint8_t* data, uint32_t len,
					   const std::vector<uint8_t>& response,
					   const EnumOptions& opt) {
	const Verdict v = ResponseVerdict(message, data, len, response, opt);
	if(v == Verdict::Undecodable) {
		g_undecodable.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	return v == Verdict::Forbidden;
}

uint64_t UndecodableResponses() {
	return g_undecodable.load(std::memory_order_relaxed);
}

bool DecodeActivation(uint8_t message, const uint8_t* data, uint32_t len,
					  const std::vector<uint8_t>& response,
					  const EnumOptions& opt, ActivationRead& out) {
	out = ActivationRead{};
	auto canon = [&](uint32_t c) {
		return opt.db ? opt.db->Canonical(c) : c;
	};
	auto undecodable = [] {
		g_undecodable.fetch_add(1, std::memory_order_relaxed);
		return false;
	};

	switch(message) {
	case MSG_SELECT_IDLECMD: {
		if(response.size() != 4)
			return false;
		int32_t v = 0;
		std::memcpy(&v, response.data(), 4);
		const uint32_t t = static_cast<uint32_t>(v) & 0xffffu;
		const uint32_t s = static_cast<uint32_t>(v) >> 16;
		if(t != 5)
			return false;   // summon / set / change phase: not an activation
		Reader r(data, len);
		r.Get<uint8_t>();
		const uint32_t strides[5] = { 10, 10, 7, 10, 10 };
		for(uint32_t g = 0; g < 5; ++g) {
			uint32_t n = r.Get<uint32_t>();
			for(uint32_t i = 0; i < n && r.Ok(); ++i)
				r.Skip(strides[g]);
		}
		uint32_t n_act = r.Get<uint32_t>();
		if(!r.Ok() || s >= n_act)
			return undecodable();
		// The activable entry: code u32 | controller u8 | zone u8 | sequence u32
		//                     | description u64 | client mode u8
		for(uint32_t i = 0; i < s; ++i)
			r.Skip(4 + 1 + 1 + 4 + 8 + 1);
		out.code = canon(r.Get<uint32_t>());
		r.Skip(1);
		out.location = r.Get<uint8_t>();
		out.sequence = r.Get<uint32_t>();
		out.desc = r.Get<uint64_t>();
		if(!r.Ok())
			return undecodable();
		out.has_zone = true;
		return true;
	}
	case MSG_SELECT_BATTLECMD: {
		if(response.size() != 4)
			return false;
		int32_t v = 0;
		std::memcpy(&v, response.data(), 4);
		const uint32_t t = static_cast<uint32_t>(v) & 0xffffu;
		const uint32_t s = static_cast<uint32_t>(v) >> 16;
		if(t != 0)
			return false;   // attack / change phase
		Reader r(data, len);
		r.Get<uint8_t>();
		uint32_t n_act = r.Get<uint32_t>();
		if(!r.Ok() || s >= n_act)
			return undecodable();
		for(uint32_t i = 0; i < s; ++i)
			r.Skip(4 + 1 + 1 + 4 + 8 + 1);
		out.code = canon(r.Get<uint32_t>());
		r.Skip(1);
		out.location = r.Get<uint8_t>();
		out.sequence = r.Get<uint32_t>();
		out.desc = r.Get<uint64_t>();
		if(!r.Ok())
			return undecodable();
		out.has_zone = true;
		return true;
	}
	case MSG_SELECT_CHAIN: {
		if(response.size() != 4)
			return false;
		int32_t v = 0;
		std::memcpy(&v, response.data(), 4);
		if(v < 0)
			return false;   // "do not chain"
		Reader r(data, len);
		r.Get<uint8_t>();
		r.Get<uint8_t>();
		r.Get<uint8_t>();          // forced
		r.Get<uint32_t>(); r.Get<uint32_t>();
		uint32_t n = r.Get<uint32_t>();
		if(!r.Ok() || static_cast<uint32_t>(v) >= n)
			return undecodable();
		for(int32_t i = 0; i < v; ++i)
			r.Skip(4 + kLocInfo + 8 + 1);
		out.code = canon(r.Get<uint32_t>());
		r.Skip(1);
		out.location = r.Get<uint8_t>();
		out.sequence = r.Get<uint32_t>();
		r.Skip(4);                 // position
		out.desc = r.Get<uint64_t>();
		if(!r.Ok())
			return undecodable();
		out.has_zone = true;
		return true;
	}
	case MSG_SELECT_EFFECTYN:
	case MSG_SELECT_YESNO: {
		if(response.size() != 4)
			return false;
		int32_t v = 0;
		std::memcpy(&v, response.data(), 4);
		if(v != 1)
			return false;   // a "no" employs no operator
		Reader r(data, len);
		r.Get<uint8_t>();
		if(message == MSG_SELECT_EFFECTYN) {
			out.code = canon(r.Get<uint32_t>());
			r.Get<uint8_t>();          // controller
			out.location = r.Get<uint8_t>();
			out.sequence = r.Get<uint32_t>();
			r.Skip(4);                 // position
			out.has_zone = true;
		}
		out.desc = r.Get<uint64_t>();
		if(!r.Ok())
			return undecodable();
		// The YESNO carries NO code: the description carries it instead.
		if(!out.code && out.desc)
			out.code = canon(CodeFromDesc(out.desc, opt.db));
		return true;
	}
	default:
		return false;
	}
}

} // namespace solver
