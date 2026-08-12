#include "prompt.h"

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "ocgapi_constants.h"

namespace solver {
namespace {

// Lecteur qui signale le debordement au lieu de lire hors du tampon : les
// charges utiles varient d'une version du core a l'autre.
class Buf {
public:
	Buf(const uint8_t* d, uint32_t n) : data(d), len(n) {}
	template<typename T> T Get() {
		T v{};
		if(pos + sizeof(T) > len) {
			ok = false;
			return v;
		}
		std::memcpy(&v, data + pos, sizeof(T));
		pos += sizeof(T);
		return v;
	}
	void Skip(uint32_t n) {
		if(pos + n > len)
			ok = false;
		else
			pos += n;
	}
	bool Ok() const { return ok; }

private:
	const uint8_t* data;
	uint32_t len, pos{ 0 };
	bool ok{ true };
};

// Nombre de sous-ensembles de taille lo..hi parmi n. Sature plutot que de
// deborder : au-dela, seul l'ordre de grandeur compte.
long double Subsets(uint32_t n, uint32_t lo, uint32_t hi) {
	if(hi > n)
		hi = n;
	if(lo > hi)
		return 0;
	long double total = 0, c = 1;
	for(uint32_t k = 0; k <= hi; ++k) {
		if(k >= lo)
			total += c;
		c = c * static_cast<long double>(n - k) / static_cast<long double>(k + 1);
	}
	return total;
}

std::string Detail(std::initializer_list<std::pair<const char*, long long>> kv) {
	std::string s;
	for(const auto& [k, v] : kv) {
		if(!s.empty())
			s += " ";
		s += k;
		s += "=";
		s += std::to_string(v);
	}
	return s;
}

constexpr uint32_t kLocInfoSize = 1 + 1 + 4 + 4;

} // namespace

bool IsPrompt(uint8_t m) {
	switch(m) {
	case MSG_SELECT_BATTLECMD: case MSG_SELECT_IDLECMD:
	case MSG_SELECT_EFFECTYN:  case MSG_SELECT_YESNO:
	case MSG_SELECT_OPTION:    case MSG_SELECT_CARD:
	case MSG_SELECT_CHAIN:     case MSG_SELECT_PLACE:
	case MSG_SELECT_POSITION:  case MSG_SELECT_TRIBUTE:
	case MSG_SELECT_COUNTER:   case MSG_SELECT_SUM:
	case MSG_SELECT_DISFIELD:  case MSG_SORT_CARD:
	case MSG_SELECT_UNSELECT_CARD:
	case MSG_ANNOUNCE_RACE:    case MSG_ANNOUNCE_ATTRIB:
	case MSG_ANNOUNCE_CARD:    case MSG_ANNOUNCE_NUMBER:
		return true;
	default:
		return false;
	}
}

const char* PromptName(uint8_t m) {
	switch(m) {
	case MSG_SELECT_BATTLECMD:      return "SELECT_BATTLECMD";
	case MSG_SELECT_IDLECMD:        return "SELECT_IDLECMD";
	case MSG_SELECT_EFFECTYN:       return "SELECT_EFFECTYN";
	case MSG_SELECT_YESNO:          return "SELECT_YESNO";
	case MSG_SELECT_OPTION:         return "SELECT_OPTION";
	case MSG_SELECT_CARD:           return "SELECT_CARD";
	case MSG_SELECT_CHAIN:          return "SELECT_CHAIN";
	case MSG_SELECT_PLACE:          return "SELECT_PLACE";
	case MSG_SELECT_POSITION:       return "SELECT_POSITION";
	case MSG_SELECT_TRIBUTE:        return "SELECT_TRIBUTE";
	case MSG_SELECT_COUNTER:        return "SELECT_COUNTER";
	case MSG_SELECT_SUM:            return "SELECT_SUM";
	case MSG_SELECT_DISFIELD:       return "SELECT_DISFIELD";
	case MSG_SORT_CARD:             return "SORT_CARD";
	case MSG_SELECT_UNSELECT_CARD:  return "SELECT_UNSELECT_CARD";
	case MSG_ANNOUNCE_RACE:         return "ANNOUNCE_RACE";
	case MSG_ANNOUNCE_ATTRIB:       return "ANNOUNCE_ATTRIB";
	case MSG_ANNOUNCE_CARD:         return "ANNOUNCE_CARD";
	case MSG_ANNOUNCE_NUMBER:       return "ANNOUNCE_NUMBER";
	default:                        return "?";
	}
}

PromptInfo DecodePrompt(uint8_t message, const uint8_t* data, uint32_t len) {
	PromptInfo info;
	info.type = message;
	Buf b(data, len);

	switch(message) {
	case MSG_SELECT_IDLECMD: {
		// playerop.cpp:169 — cinq listes de cartes puis les activations.
		info.player = b.Get<uint8_t>();
		long long counts[5]{};
		long long dedup_total = 0;
		const uint32_t strides[5] = { 10, 10, 7, 10, 10 }; // repos ecrit seq sur 1 octet
		for(int g = 0; g < 5; ++g) {
			uint32_t n = b.Get<uint32_t>();
			std::set<uint32_t> distinct;
			for(uint32_t i = 0; i < n && b.Ok(); ++i) {
				distinct.insert(b.Get<uint32_t>());
				b.Skip(strides[g] - 4);
			}
			counts[g] = n;
			dedup_total += static_cast<long long>(distinct.size());
		}
		uint32_t n_act = b.Get<uint32_t>();
		std::set<std::pair<uint32_t, uint64_t>> acts;
		for(uint32_t i = 0; i < n_act && b.Ok(); ++i) {
			uint32_t code = b.Get<uint32_t>();
			b.Skip(1 + 1 + 4);
			uint64_t desc = b.Get<uint64_t>();
			b.Skip(1);
			acts.insert({ code, desc });
		}
		uint8_t to_bp = b.Get<uint8_t>();
		uint8_t to_ep = b.Get<uint8_t>();
		uint8_t shuffle = b.Get<uint8_t>();
		if(!b.Ok())
			return info;
		long long extras = to_bp + to_ep + shuffle;
		info.raw = static_cast<long double>(counts[0] + counts[1] + counts[2] +
											counts[3] + counts[4] + n_act + extras);
		info.dedup = static_cast<long double>(dedup_total +
											  static_cast<long long>(acts.size()) + extras);
		info.detail = Detail({ { "summon", counts[0] }, { "spsummon", counts[1] },
							   { "repos", counts[2] }, { "mset", counts[3] },
							   { "sset", counts[4] }, { "activate", n_act } });
		return info;
	}
	case MSG_SELECT_BATTLECMD: {
		info.player = b.Get<uint8_t>();
		uint32_t n_act = b.Get<uint32_t>();
		for(uint32_t i = 0; i < n_act && b.Ok(); ++i)
			b.Skip(4 + 1 + 1 + 4 + 8 + 1);
		uint32_t n_atk = b.Get<uint32_t>();
		for(uint32_t i = 0; i < n_atk && b.Ok(); ++i)
			b.Skip(4 + 1 + 1 + 4 + 1);
		uint8_t to_m2 = b.Get<uint8_t>();
		uint8_t to_ep = b.Get<uint8_t>();
		if(!b.Ok())
			return info;
		info.raw = info.dedup = static_cast<long double>(n_act + n_atk + to_m2 + to_ep);
		info.detail = Detail({ { "activate", n_act }, { "attack", n_atk } });
		return info;
	}
	case MSG_SELECT_EFFECTYN:
	case MSG_SELECT_YESNO:
		info.player = b.Get<uint8_t>();
		info.raw = info.dedup = 2;
		return info;

	case MSG_SELECT_OPTION: {
		info.player = b.Get<uint8_t>();
		uint8_t n = b.Get<uint8_t>();
		if(!b.Ok())
			return info;
		info.raw = info.dedup = n;
		info.detail = Detail({ { "options", n } });
		return info;
	}
	case MSG_SELECT_CARD:
	case MSG_SELECT_TRIBUTE: {
		info.player = b.Get<uint8_t>();
		uint8_t cancelable = b.Get<uint8_t>();
		uint32_t lo = b.Get<uint32_t>();
		uint32_t hi = b.Get<uint32_t>();
		uint32_t n = b.Get<uint32_t>();
		std::set<uint32_t> distinct;
		// SELECT_TRIBUTE ecrit sequence sur 4 octets + release_param, pas un
		// loc_info complet (playerop.cpp:672).
		const uint32_t stride = (message == MSG_SELECT_CARD) ? kLocInfoSize : 7;
		for(uint32_t i = 0; i < n && b.Ok(); ++i) {
			distinct.insert(b.Get<uint32_t>());
			b.Skip(stride);
		}
		if(!b.Ok())
			return info;
		long double cancel = cancelable ? 1 : 0;
		info.raw = Subsets(n, lo, hi) + cancel;
		info.dedup = Subsets(static_cast<uint32_t>(distinct.size()), lo, hi) + cancel;
		info.detail = Detail({ { "n", n }, { "min", lo }, { "max", hi },
							   { "distincts", static_cast<long long>(distinct.size()) } });
		return info;
	}
	case MSG_SELECT_UNSELECT_CARD: {
		info.player = b.Get<uint8_t>();
		uint8_t finishable = b.Get<uint8_t>();
		uint8_t cancelable = b.Get<uint8_t>();
		b.Get<uint32_t>();                  // min
		b.Get<uint32_t>();                  // max
		uint32_t n = b.Get<uint32_t>();
		std::set<uint32_t> distinct;
		for(uint32_t i = 0; i < n && b.Ok(); ++i) {
			distinct.insert(b.Get<uint32_t>());
			b.Skip(kLocInfoSize);
		}
		uint32_t n_un = b.Get<uint32_t>();
		if(!b.Ok())
			return info;
		// Une seule carte est choisie par tour de boucle, pas un sous-ensemble.
		long double stop = (finishable || cancelable) ? 1 : 0;
		info.raw = static_cast<long double>(n + n_un) + stop;
		info.dedup = static_cast<long double>(distinct.size() + n_un) + stop;
		info.detail = Detail({ { "selectables", n }, { "unselectables", n_un } });
		return info;
	}
	case MSG_SELECT_CHAIN: {
		info.player = b.Get<uint8_t>();
		uint8_t spe = b.Get<uint8_t>();
		uint8_t forced = b.Get<uint8_t>();
		b.Get<uint32_t>();                  // hint timing joueur
		b.Get<uint32_t>();                  // hint timing adversaire
		uint32_t n = b.Get<uint32_t>();
		std::set<std::pair<uint32_t, uint64_t>> distinct;
		for(uint32_t i = 0; i < n && b.Ok(); ++i) {
			uint32_t code = b.Get<uint32_t>();
			b.Skip(kLocInfoSize);
			uint64_t desc = b.Get<uint64_t>();
			b.Skip(1);
			distinct.insert({ code, desc });
		}
		if(!b.Ok())
			return info;
		long double pass = forced ? 0 : 1;
		info.raw = static_cast<long double>(n) + pass;
		info.dedup = static_cast<long double>(distinct.size()) + pass;
		info.detail = Detail({ { "chains", n }, { "forced", forced }, { "spe", spe } });
		return info;
	}
	case MSG_SELECT_PLACE:
	case MSG_SELECT_DISFIELD: {
		info.player = b.Get<uint8_t>();
		uint8_t count = b.Get<uint8_t>();
		uint32_t flag = b.Get<uint32_t>();
		if(!b.Ok())
			return info;
		// Un bit a 1 = zone interdite (playerop.cpp:590).
		uint32_t free_zones = 0;
		for(int owner = 0; owner < 2; ++owner) {
			for(int seq = 0; seq < 7; ++seq)
				if(!(flag & (1u << (seq + owner * 16))))
					++free_zones;
			for(int seq = 0; seq < 8; ++seq)
				if(!(flag & (1u << (seq + 8 + owner * 16))))
					++free_zones;
		}
		info.raw = info.dedup = Subsets(free_zones, count, count);
		info.detail = Detail({ { "count", count }, { "zones_libres", free_zones } });
		return info;
	}
	case MSG_SELECT_POSITION: {
		info.player = b.Get<uint8_t>();
		b.Get<uint32_t>();                  // code
		uint8_t pos = b.Get<uint8_t>() & 0xf;
		if(!b.Ok())
			return info;
		int n = 0;
		for(int i = 0; i < 4; ++i)
			if(pos & (1 << i))
				++n;
		info.raw = info.dedup = n;
		info.detail = Detail({ { "positions", pos } });
		return info;
	}
	case MSG_SELECT_COUNTER: {
		info.player = b.Get<uint8_t>();
		b.Get<uint16_t>();                  // type de compteur
		uint16_t count = b.Get<uint16_t>();
		uint32_t n = b.Get<uint32_t>();
		if(!b.Ok())
			return info;
		info.raw = info.dedup = n;
		info.detail = Detail({ { "cartes", n }, { "count", count } });
		return info;
	}
	case MSG_SELECT_SUM: {
		info.player = b.Get<uint8_t>();
		uint8_t mode = b.Get<uint8_t>();
		b.Get<uint32_t>();                  // acc
		b.Get<uint32_t>();                  // min
		b.Get<uint32_t>();                  // max
		uint32_t n_must = b.Get<uint32_t>();
		for(uint32_t i = 0; i < n_must && b.Ok(); ++i)
			b.Skip(4 + kLocInfoSize + 4);
		uint32_t n = b.Get<uint32_t>();
		if(!b.Ok())
			return info;
		// La contrainte de somme n'est verifiable qu'en interrogeant le core :
		// on majore par tous les sous-ensembles non vides.
		info.raw = info.dedup = Subsets(n, 1, n);
		info.detail = Detail({ { "n", n }, { "must", n_must }, { "mode", mode } });
		return info;
	}
	case MSG_SORT_CARD: {
		info.player = b.Get<uint8_t>();
		uint32_t n = b.Get<uint32_t>();
		if(!b.Ok())
			return info;
		long double fact = 1;
		for(uint32_t i = 2; i <= n && i <= 12; ++i)
			fact *= i;
		info.raw = fact;
		info.dedup = 1;   // l'ordre du deck n'entre pas dans l'equivalence de board
		info.detail = Detail({ { "n", n } });
		return info;
	}
	default:
		return info;      // ANNOUNCE_* : espace de valeurs, pas une liste
	}
}

} // namespace solver
