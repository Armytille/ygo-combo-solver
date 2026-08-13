#include "enumerate.h"

#include <algorithm>
#include <cstring>

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

// Ecrivent EN PLACE dans la reponse d'un Choice reutilise : resize() sur un
// vecteur deja capacitaire n'alloue pas.
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

uint64_t EdgeOf(uint8_t message, std::initializer_list<uint64_t> parts) {
	uint64_t h = message * 0x100000001b3ull;
	for(uint64_t p : parts)
		h = Mix(h, p);
	return h;
}

// Label construit seulement si demande : chaque label est une allocation de
// chaine par choix, et les chemins chauds n'en lisent aucun.
void SetLabel(Choice& c, const EnumOptions& opt, const char* prefix,
			  uint64_t num, bool with_num = true) {
	if(!opt.labels)
		return;
	c.label = prefix;
	if(with_num)
		c.label += std::to_string(num);
}

// Sous-ensembles de `n` elements de taille lo..hi, plafonnes, livres au
// callback un par un — aucun stockage intermediaire. On privilegie les tailles
// extremes : une selection minimale (economie de ressources) et une selection
// maximale sont les deux qui portent l'essentiel de l'information.
template<typename F>
void ForEachSubset(uint32_t n, uint32_t lo, uint32_t hi, uint32_t cap, F&& f) {
	static thread_local std::vector<uint8_t> cur;
	hi = (std::min)(hi, n);
	if(lo > hi)
		return;
	uint32_t emitted = 0;
	for(uint32_t k = lo; k <= hi && emitted < cap; ++k) {
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
			if(++emitted >= cap)
				return;
			// combinaison suivante en ordre lexicographique
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

void EnumerateRaw(uint8_t message, const uint8_t* data, uint32_t len,
				  const EnumOptions& opt, ChoiceList& out) {
	Reader r(data, len);
	// Les codes lus dans le prompt sont les codes IMPRIMES : deux illustrations
	// de la meme carte n'y portent pas le meme nombre. Toute arete se construit
	// donc sur le code canonique, sinon elle ne designe pas une carte mais un
	// exemplaire — inutilisable pour transposer, et faux pour dedupliquer.
	auto canon = [&opt](uint32_t code) {
		return opt.db ? opt.db->Canonical(code) : code;
	};
	// Deduplication sans std::set : les listes sont courtes (quelques dizaines),
	// la recherche lineaire dans un tampon reutilise bat le nid d'allocations.
	static thread_local std::vector<uint32_t> seen_codes;
	static thread_local std::vector<std::pair<uint32_t, uint64_t>> seen_pairs;
	static thread_local std::vector<uint32_t> codes;

	switch(message) {
	case MSG_SELECT_IDLECMD: {
		r.Get<uint8_t>();   // playerid
		// t : 0 invocation, 1 invocation speciale, 2 changement de position,
		// 3 pose de monstre, 4 pose de magie/piege, 5 activation.
		// La reponse est t | (s << 16)  (playerop.cpp:141)
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
			r.Skip(1);                       // controleur
			uint8_t loc = r.Get<uint8_t>();  // zone d'ou la carte s'active
			r.Skip(4);                       // sequence
			uint64_t desc = r.Get<uint64_t>();
			r.Skip(1);
			// Activation interdite depuis cette zone : le choix n'existe pas.
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
		r.Get<uint8_t>();   // melanger la main : sans effet sur le board
		if(!r.Ok()) {
			out.Clear();
			return;
		}
		if(opt.allow_phase_change) {
			if(to_bp) {
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
			r.Skip(1 + 1 + 4 + 1);
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
		if(to_m2) {
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
		Choice& y = out.Emit();
		PutInt32(y.response, 1);
		y.edge = EdgeOf(message, { 1 });
		SetLabel(y, opt, "oui", 0, false);
		Choice& n = out.Emit();
		PutInt32(n.response, 0);
		n.edge = EdgeOf(message, { 0 });
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
			// --no-chain : cette carte ne se chaine jamais (ses declencheurs
			// FORCES ne passent pas par ici, forced est exempte ci-dessous).
			// L'option disparait, "ne pas chainer" demeure.
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
			// La carte engagee : sans elle, les effets RAPIDES (le rip
			// d'Omega s'active en fenetre de chaine) echappaient au biais des
			// indices — les invocations etaient biaisees, jamais les
			// activations en chaine.
			c.card = code;
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
		// Les cartes de meme code sont interchangeables : on ne garde qu'un
		// representant de chaque code avant de former les sous-ensembles.
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
						  if(opt.labels)
							  c.label = "choisir " + std::to_string(k) +
										" carte(s)";
					  });
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
		codes.clear();
		for(uint32_t i = 0; i < n && r.Ok(); ++i) {
			codes.push_back(canon(r.Get<uint32_t>()));
			r.Skip(kLocInfo);
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
		seen_codes.clear();
		for(uint32_t i = 0; i < codes.size(); ++i) {
			if(opt.dedup_by_code) {
				if(std::find(seen_codes.begin(), seen_codes.end(), codes[i]) !=
				   seen_codes.end())
					continue;
				seen_codes.push_back(codes[i]);
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
		// Un bit a 1 = zone interdite (playerop.cpp:590). 30 emplacements au
		// plus : un tableau de pile suffit.
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
			// Une seule zone representative par (proprietaire, type de zone).
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
		// On ne pose qu'une carte a la fois dans l'immense majorite des cas.
		if(count == 1) {
			for(uint32_t i = 0; i < n_free; ++i) {
				const Slot& s = free_slots[i];
				Choice& c = out.Emit();
				c.response.resize(3);
				c.response[0] = s.owner;
				c.response[1] = s.loc;
				c.response[2] = s.seq;
				c.edge = EdgeOf(message, { s.owner, s.loc, s.seq });
				// L'identite de plan ignore la colonne : deux placements dans
				// deux colonnes libres realisent la meme intention.
				c.plan_key = EdgeOf(message, { s.owner, s.loc });
				SetLabel(c, opt, "zone ", s.seq);
			}
		} else {
			// Placement multiple : on prend les premieres zones libres.
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
		// La CARTE fait partie de l'identite du choix. Sans elle, "ATK" et
		// "DEF" sont deux coups globaux : la politique NRPA apprend UN poids
		// pour toutes les positions de toutes les cartes — mesure : six
		// monstres en DEF la ou la cible en veut cinq en ATK — et le
		// repertoire perd l'intention par carte de la reference.
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
		// La contrainte de somme n'est verifiable que par le core : on propose
		// tous les sous-ensembles, les invalides seront rejetes (MSG_RETRY) et
		// la branche abandonnee.
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
					  });
		return;
	}

	case MSG_SELECT_COUNTER:
	case MSG_SORT_CARD:
	case MSG_ANNOUNCE_RACE:
	case MSG_ANNOUNCE_ATTRIB:
	case MSG_ANNOUNCE_CARD:
	case MSG_ANNOUNCE_NUMBER: {
		// Espaces de valeurs, pas des listes : une enumeration naive serait
		// enorme et surtout non pertinente pour l'egalite de board. On se
		// contente de la reponse par defaut.
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
	out.Clear();
	EnumerateRaw(message, data, len, opt, out);
	// Par defaut l'identite de plan est l'arete elle-meme ; seuls les prompts
	// qui la distinguent explicitement (choix de zone) la renseignent.
	for(Choice& c : out)
		if(!c.plan_key)
			c.plan_key = c.edge;
}

std::vector<Choice> Enumerate(uint8_t message, const uint8_t* data, uint32_t len,
							  const EnumOptions& opt) {
	static thread_local ChoiceList scratch;
	EnumOptions o = opt;
	o.labels = true;   // les appelants froids lisent les labels
	EnumerateInto(message, data, len, o, scratch);
	return std::vector<Choice>(scratch.begin(), scratch.end());
}

bool DefaultResponse(uint8_t message, const uint8_t* data, uint32_t len,
					 std::vector<uint8_t>& out) {
	Reader r(data, len);
	switch(message) {
	case MSG_SORT_CARD: {
		// Ordre identite : l'ordre du deck n'entre pas dans l'equivalence de
		// board retenue.
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
		// Retirer les compteurs sur les premieres cartes disponibles.
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
		int32_t v = 0;   // premier choix propose
		std::memcpy(out.data(), &v, 4);
		return true;
	}
	case MSG_ANNOUNCE_RACE:
	case MSG_ANNOUNCE_ATTRIB: {
		// Il faut annoncer un bit parmi ceux autorises ; sans decodage fin, on
		// laisse la branche echouer plutot que de risquer une reponse illegale.
		return false;
	}
	default:
		return false;
	}
}

bool ResponseForbidden(uint8_t message, const uint8_t* data, uint32_t len,
					   const std::vector<uint8_t>& response,
					   const EnumOptions& opt) {
	// --no-chain : une reponse ENREGISTREE qui chaine une carte interdite est
	// rattrapee ici (mode reparation, ou la reponse de la reference est
	// candidate a cout zero sans passer par l'enumerateur).
	if(message == MSG_SELECT_CHAIN && response.size() == 4 && opt.no_chain &&
	   !opt.no_chain->empty()) {
		int32_t v = 0;
		std::memcpy(&v, response.data(), 4);
		if(v < 0)
			return false;   // "ne pas chainer"
		Reader r(data, len);
		r.Get<uint8_t>();
		r.Get<uint8_t>();
		uint8_t forced = r.Get<uint8_t>();
		r.Get<uint32_t>();
		r.Get<uint32_t>();
		uint32_t n = r.Get<uint32_t>();
		if(forced || static_cast<uint32_t>(v) >= n || !r.Ok())
			return false;
		for(int32_t i = 0; i < v; ++i)
			r.Skip(4 + kLocInfo + 8 + 1);
		uint32_t code = r.Get<uint32_t>();
		if(!r.Ok())
			return false;
		if(opt.db)
			code = opt.db->Canonical(code);
		return std::find(opt.no_chain->begin(), opt.no_chain->end(), code) !=
			   opt.no_chain->end();
	}
	if(!opt.no_activate || opt.no_activate->empty())
		return false;
	if(message != MSG_SELECT_IDLECMD || response.size() != 4)
		return false;
	int32_t v = 0;
	std::memcpy(&v, response.data(), 4);
	uint32_t t = static_cast<uint32_t>(v) & 0xffffu;
	uint32_t s = static_cast<uint32_t>(v) >> 16;
	if(t != 5)
		return false;   // seule la famille "activer" est couverte
	// Rejouer le decodage du prompt jusqu'a l'entree designee.
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
		return false;
	for(uint32_t i = 0; i < s; ++i)
		r.Skip(4 + 1 + 1 + 4 + 8 + 1);
	uint32_t code = r.Get<uint32_t>();
	r.Skip(1);
	uint8_t loc = r.Get<uint8_t>();
	if(!r.Ok())
		return false;
	if(opt.db)
		code = opt.db->Canonical(code);
	auto it = opt.no_activate->find(code);
	return it != opt.no_activate->end() && (it->second & loc) != 0;
}

} // namespace solver
