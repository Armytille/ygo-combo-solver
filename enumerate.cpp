#include "enumerate.h"

#include <algorithm>
#include <cstring>
#include <set>

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

std::vector<uint8_t> Int32Response(int32_t v) {
	std::vector<uint8_t> r(4);
	std::memcpy(r.data(), &v, 4);
	return r;
}

// [int32 type=2][uint32 count][uint8 index...]  (playerop.cpp:258)
std::vector<uint8_t> CardIndexResponse(const std::vector<uint8_t>& idx) {
	std::vector<uint8_t> r(8 + idx.size());
	int32_t type = 2;
	uint32_t count = static_cast<uint32_t>(idx.size());
	std::memcpy(r.data(), &type, 4);
	std::memcpy(r.data() + 4, &count, 4);
	std::memcpy(r.data() + 8, idx.data(), idx.size());
	return r;
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

// Sous-ensembles de `n` elements de taille lo..hi, plafonnes. On privilegie les
// tailles extremes : une selection minimale (economie de ressources) et une
// selection maximale sont les deux qui portent l'essentiel de l'information.
void EnumerateSubsets(uint32_t n, uint32_t lo, uint32_t hi, uint32_t cap,
					  std::vector<std::vector<uint8_t>>& out) {
	hi = (std::min)(hi, n);
	if(lo > hi)
		return;
	for(uint32_t k = lo; k <= hi && out.size() < cap; ++k) {
		if(k == 0) {
			out.emplace_back();
			continue;
		}
		std::vector<uint8_t> cur(k);
		for(uint32_t i = 0; i < k; ++i)
			cur[i] = static_cast<uint8_t>(i);
		for(;;) {
			out.push_back(cur);
			if(out.size() >= cap)
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

} // namespace

namespace {

std::vector<Choice> EnumerateRaw(uint8_t message, const uint8_t* data,
								 uint32_t len, const EnumOptions& opt) {
	std::vector<Choice> out;
	Reader r(data, len);
	// Les codes lus dans le prompt sont les codes IMPRIMES : deux illustrations
	// de la meme carte n'y portent pas le meme nombre. Toute arete se construit
	// donc sur le code canonique, sinon elle ne designe pas une carte mais un
	// exemplaire — inutilisable pour transposer, et faux pour dedupliquer.
	auto canon = [&opt](uint32_t code) {
		return opt.db ? opt.db->Canonical(code) : code;
	};

	switch(message) {
	case MSG_SELECT_IDLECMD: {
		r.Get<uint8_t>();   // playerid
		// t : 0 invocation, 1 invocation speciale, 2 changement de position,
		// 3 pose de monstre, 4 pose de magie/piege, 5 activation.
		// La reponse est t | (s << 16)  (playerop.cpp:141)
		static const char* kNames[5] = { "invoquer", "inv.speciale", "reposition",
										 "poser-mon", "poser-st" };
		const uint32_t strides[5] = { 10, 10, 7, 10, 10 };
		for(uint32_t g = 0; g < 5; ++g) {
			uint32_t n = r.Get<uint32_t>();
			std::set<uint32_t> seen;
			for(uint32_t i = 0; i < n && r.Ok(); ++i) {
				uint32_t code = canon(r.Get<uint32_t>());
				r.Skip(strides[g] - 4);
				if(opt.dedup_by_code && !seen.insert(code).second)
					continue;
				Choice c;
				c.response = Int32Response(static_cast<int32_t>(g | (i << 16)));
				c.edge = EdgeOf(message, { g, code, opt.dedup_by_code ? 0u : i });
				c.label = std::string(kNames[g]) + " " + std::to_string(code);
				out.push_back(std::move(c));
			}
		}
		uint32_t n_act = r.Get<uint32_t>();
		std::set<std::pair<uint32_t, uint64_t>> seen_act;
		for(uint32_t i = 0; i < n_act && r.Ok(); ++i) {
			uint32_t code = canon(r.Get<uint32_t>());
			r.Skip(1 + 1 + 4);
			uint64_t desc = r.Get<uint64_t>();
			r.Skip(1);
			if(opt.dedup_by_code && !seen_act.insert({ code, desc }).second)
				continue;
			Choice c;
			c.response = Int32Response(static_cast<int32_t>(5u | (i << 16)));
			c.edge = EdgeOf(message, { 5, code, desc });
			c.label = "activer " + std::to_string(code);
			out.push_back(std::move(c));
		}
		uint8_t to_bp = r.Get<uint8_t>();
		uint8_t to_ep = r.Get<uint8_t>();
		r.Get<uint8_t>();   // melanger la main : sans effet sur le board
		if(!r.Ok())
			return {};
		if(opt.allow_phase_change) {
			if(to_bp) {
				Choice c;
				c.response = Int32Response(6);
				c.edge = EdgeOf(message, { 6 });
				c.label = "-> Battle Phase";
				out.push_back(std::move(c));
			}
			if(to_ep) {
				Choice c;
				c.response = Int32Response(7);
				c.edge = EdgeOf(message, { 7 });
				c.label = "-> End Phase";
				out.push_back(std::move(c));
			}
		}
		return out;
	}

	case MSG_SELECT_BATTLECMD: {
		r.Get<uint8_t>();
		uint32_t n_act = r.Get<uint32_t>();
		for(uint32_t i = 0; i < n_act && r.Ok(); ++i) {
			uint32_t code = canon(r.Get<uint32_t>());
			r.Skip(1 + 1 + 4);
			uint64_t desc = r.Get<uint64_t>();
			r.Skip(1);
			Choice c;
			c.response = Int32Response(static_cast<int32_t>(0u | (i << 16)));
			c.edge = EdgeOf(message, { 0, code, desc });
			c.label = "activer " + std::to_string(code);
			out.push_back(std::move(c));
		}
		uint32_t n_atk = r.Get<uint32_t>();
		for(uint32_t i = 0; i < n_atk && r.Ok(); ++i) {
			uint32_t code = canon(r.Get<uint32_t>());
			r.Skip(1 + 1 + 4 + 1);
			Choice c;
			c.response = Int32Response(static_cast<int32_t>(1u | (i << 16)));
			c.edge = EdgeOf(message, { 1, code });
			c.label = "attaquer avec " + std::to_string(code);
			out.push_back(std::move(c));
		}
		uint8_t to_m2 = r.Get<uint8_t>();
		uint8_t to_ep = r.Get<uint8_t>();
		if(!r.Ok())
			return {};
		if(to_m2) out.push_back({ Int32Response(2), EdgeOf(message, { 2 }), "-> Main 2" });
		if(to_ep) out.push_back({ Int32Response(3), EdgeOf(message, { 3 }), "-> End Phase" });
		return out;
	}

	case MSG_SELECT_EFFECTYN:
	case MSG_SELECT_YESNO:
		out.push_back({ Int32Response(1), EdgeOf(message, { 1 }), "oui" });
		out.push_back({ Int32Response(0), EdgeOf(message, { 0 }), "non" });
		return out;

	case MSG_SELECT_OPTION: {
		r.Get<uint8_t>();
		uint8_t n = r.Get<uint8_t>();
		for(uint8_t i = 0; i < n && r.Ok(); ++i) {
			uint64_t desc = r.Get<uint64_t>();
			out.push_back({ Int32Response(i), EdgeOf(message, { desc }),
							"option " + std::to_string(i) });
		}
		return out;
	}

	case MSG_SELECT_CHAIN: {
		r.Get<uint8_t>();
		r.Get<uint8_t>();                 // spe_count
		uint8_t forced = r.Get<uint8_t>();
		r.Get<uint32_t>(); r.Get<uint32_t>();
		uint32_t n = r.Get<uint32_t>();
		std::set<std::pair<uint32_t, uint64_t>> seen;
		for(uint32_t i = 0; i < n && r.Ok(); ++i) {
			uint32_t code = canon(r.Get<uint32_t>());
			r.Skip(kLocInfo);
			uint64_t desc = r.Get<uint64_t>();
			r.Skip(1);
			if(opt.dedup_by_code && !seen.insert({ code, desc }).second)
				continue;
			out.push_back({ Int32Response(static_cast<int32_t>(i)),
							EdgeOf(message, { code, desc }),
							"chainer " + std::to_string(code) });
		}
		if(!r.Ok())
			return {};
		if(!forced)
			out.push_back({ Int32Response(-1), EdgeOf(message, { uint64_t(-1) }),
							"ne pas chainer" });
		return out;
	}

	case MSG_SELECT_CARD:
	case MSG_SELECT_TRIBUTE: {
		r.Get<uint8_t>();
		uint8_t cancelable = r.Get<uint8_t>();
		uint32_t lo = r.Get<uint32_t>();
		uint32_t hi = r.Get<uint32_t>();
		uint32_t n = r.Get<uint32_t>();
		std::vector<uint32_t> codes;
		const uint32_t stride = (message == MSG_SELECT_CARD) ? kLocInfo : 7;
		for(uint32_t i = 0; i < n && r.Ok(); ++i) {
			codes.push_back(canon(r.Get<uint32_t>()));
			r.Skip(stride);
		}
		if(!r.Ok())
			return {};
		// Les cartes de meme code sont interchangeables : on ne garde qu'un
		// representant de chaque code avant de former les sous-ensembles.
		std::vector<uint8_t> pool;
		std::set<uint32_t> seen;
		for(uint32_t i = 0; i < codes.size(); ++i)
			if(!opt.dedup_by_code || seen.insert(codes[i]).second)
				pool.push_back(static_cast<uint8_t>(i));

		std::vector<std::vector<uint8_t>> subsets;
		EnumerateSubsets(static_cast<uint32_t>(pool.size()), lo, hi,
						 opt.max_subsets, subsets);
		for(const auto& s : subsets) {
			std::vector<uint8_t> idx;
			uint64_t h = 0;
			for(uint8_t p : s) {
				idx.push_back(pool[p]);
				h = Mix(h, codes[pool[p]]);
			}
			out.push_back({ CardIndexResponse(idx), EdgeOf(message, { h, idx.size() }),
							"choisir " + std::to_string(idx.size()) + " carte(s)" });
		}
		if(cancelable)
			out.push_back({ Int32Response(-1), EdgeOf(message, { uint64_t(-1) }),
							"annuler" });
		return out;
	}

	case MSG_SELECT_UNSELECT_CARD: {
		r.Get<uint8_t>();
		uint8_t finishable = r.Get<uint8_t>();
		uint8_t cancelable = r.Get<uint8_t>();
		r.Get<uint32_t>(); r.Get<uint32_t>();
		uint32_t n = r.Get<uint32_t>();
		std::vector<uint32_t> codes;
		for(uint32_t i = 0; i < n && r.Ok(); ++i) {
			codes.push_back(canon(r.Get<uint32_t>()));
			r.Skip(kLocInfo);
		}
		uint32_t n_un = r.Get<uint32_t>();
		if(!r.Ok())
			return {};
		// [int32 1][int32 index]  (playerop.cpp:439)
		auto make = [&](uint32_t index, uint64_t key, const char* what) {
			std::vector<uint8_t> resp(8);
			int32_t one = 1, idx = static_cast<int32_t>(index);
			std::memcpy(resp.data(), &one, 4);
			std::memcpy(resp.data() + 4, &idx, 4);
			out.push_back({ std::move(resp), EdgeOf(message, { key }), what });
		};
		std::set<uint32_t> seen;
		for(uint32_t i = 0; i < codes.size(); ++i)
			if(!opt.dedup_by_code || seen.insert(codes[i]).second)
				make(i, codes[i], "selectionner");
		for(uint32_t i = 0; i < n_un; ++i)
			make(static_cast<uint32_t>(codes.size()) + i, 0x8000000ull + i,
				 "deselectionner");
		if(finishable || cancelable)
			out.push_back({ Int32Response(-1), EdgeOf(message, { uint64_t(-1) }),
							"terminer" });
		return out;
	}

	case MSG_SELECT_PLACE:
	case MSG_SELECT_DISFIELD: {
		uint8_t player = r.Get<uint8_t>();
		uint8_t count = r.Get<uint8_t>();
		uint32_t flag = r.Get<uint32_t>();
		if(!r.Ok() || count == 0)
			return {};
		// Un bit a 1 = zone interdite (playerop.cpp:590).
		struct Slot { uint8_t owner, loc, seq; };
		std::vector<Slot> free_slots;
		for(uint8_t owner = 0; owner < 2; ++owner) {
			for(uint8_t seq = 0; seq < 7; ++seq)
				if(!(flag & (1u << (seq + owner * 16))))
					free_slots.push_back({ owner, LOCATION_MZONE, seq });
			for(uint8_t seq = 0; seq < 8; ++seq)
				if(!(flag & (1u << (seq + 8 + owner * 16))))
					free_slots.push_back({ owner, LOCATION_SZONE, seq });
		}
		if(opt.canonical_zones) {
			// Une seule zone representative par (proprietaire, type de zone).
			std::vector<Slot> keep;
			std::set<std::pair<uint8_t, uint8_t>> seen;
			for(const Slot& s : free_slots)
				if(seen.insert({ s.owner, s.loc }).second)
					keep.push_back(s);
			free_slots.swap(keep);
		}
		// On ne pose qu'une carte a la fois dans l'immense majorite des cas.
		if(count == 1) {
			for(const Slot& s : free_slots) {
				std::vector<uint8_t> resp{ s.owner, s.loc, s.seq };
				out.push_back({ std::move(resp),
								EdgeOf(message, { s.owner, s.loc, s.seq }),
								"zone " + std::to_string(s.seq),
								EdgeOf(message, { s.owner, s.loc }) });
			}
		} else {
			// Placement multiple : on prend les premieres zones libres.
			std::vector<uint8_t> resp;
			for(uint8_t i = 0; i < count && i < free_slots.size(); ++i) {
				resp.push_back(free_slots[i].owner);
				resp.push_back(free_slots[i].loc);
				resp.push_back(free_slots[i].seq);
			}
			if(resp.size() == size_t(count) * 3)
				out.push_back({ std::move(resp), EdgeOf(message, { count }),
								"placement multiple" });
		}
		(void)player;
		return out;
	}

	case MSG_SELECT_POSITION: {
		r.Get<uint8_t>();
		r.Get<uint32_t>();
		uint8_t pos = r.Get<uint8_t>() & 0xf;
		if(!r.Ok())
			return {};
		static const struct { uint8_t bit; const char* name; } kPos[] = {
			{ POS_FACEUP_ATTACK, "ATK" }, { POS_FACEDOWN_ATTACK, "FD-ATK" },
			{ POS_FACEUP_DEFENSE, "DEF" }, { POS_FACEDOWN_DEFENSE, "FD-DEF" },
		};
		for(const auto& p : kPos)
			if(pos & p.bit)
				out.push_back({ Int32Response(p.bit), EdgeOf(message, { p.bit }),
								p.name });
		return out;
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
		std::vector<uint32_t> codes;
		for(uint32_t i = 0; i < n && r.Ok(); ++i) {
			codes.push_back(canon(r.Get<uint32_t>()));
			r.Skip(kLocInfo + 4);
		}
		if(!r.Ok())
			return {};
		// La contrainte de somme n'est verifiable que par le core : on propose
		// tous les sous-ensembles, les invalides seront rejetes (MSG_RETRY) et
		// la branche abandonnee.
		std::vector<std::vector<uint8_t>> subsets;
		EnumerateSubsets(n, 1, n, opt.max_subsets, subsets);
		for(const auto& s : subsets) {
			uint64_t h = 0;
			for(uint8_t p : s)
				h = Mix(h, codes[p]);
			out.push_back({ CardIndexResponse(s), EdgeOf(message, { h, s.size() }),
							"somme " + std::to_string(s.size()) });
		}
		return out;
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
		std::vector<uint8_t> def;
		if(DefaultResponse(message, data, len, def))
			out.push_back({ std::move(def), EdgeOf(message, { 0 }), "defaut" });
		return out;
	}

	default:
		return out;
	}
}

} // namespace

std::vector<Choice> Enumerate(uint8_t message, const uint8_t* data, uint32_t len,
							  const EnumOptions& opt) {
	std::vector<Choice> out = EnumerateRaw(message, data, len, opt);
	// Par defaut l'identite de plan est l'arete elle-meme ; seuls les prompts
	// qui la distinguent explicitement (choix de zone) la renseignent.
	for(Choice& c : out)
		if(!c.plan_key)
			c.plan_key = c.edge;
	return out;
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
		out = Int32Response(0);   // premier choix propose
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

} // namespace solver
