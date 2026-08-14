#include "replay.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "LzmaLib.h"
}

namespace solver {
namespace {

constexpr uint8_t OLD_REPLAY_MODE = 231;
constexpr uint8_t MSG_AI_NAME = 163;
constexpr uint8_t MSG_NEW_TURN = 40;

// Curseur de lecture qui refuse de deborder plutot que de lire n'importe quoi.
class Cursor {
public:
	Cursor(const std::vector<uint8_t>& d) : data(d) {}

	bool Read(void* dst, size_t len) {
		if(!ok || pos + len > data.size()) {
			ok = false;
			return false;
		}
		if(len)
			std::memcpy(dst, data.data() + pos, len);
		pos += len;
		return true;
	}
	template<typename T> T Get() {
		T v{};
		Read(&v, sizeof(T));
		return v;
	}
	// Les noms sont stockes sur 20 UTF-16 fixes ; on ne garde que l'ASCII,
	// le solveur n'affiche ces noms qu'a titre indicatif.
	std::string Name() {
		uint16_t buf[20]{};
		if(!Read(buf, sizeof(buf)))
			return {};
		std::string out;
		for(uint16_t c : buf) {
			if(!c)
				break;
			out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
		}
		return out;
	}
	bool Eof() const { return !ok || pos >= data.size(); }
	bool Ok() const { return ok; }

private:
	const std::vector<uint8_t>& data;
	size_t pos{ 0 };
	bool ok{ true };
};

} // namespace

bool Replay::Load(const std::string& path, std::string& error) {
	FILE* fp = std::fopen(path.c_str(), "rb");
	if(!fp) {
		error = "impossible d'ouvrir " + path;
		return false;
	}
	std::fseek(fp, 0, SEEK_END);
	long size = std::ftell(fp);
	std::fseek(fp, 0, SEEK_SET);
	if(size <= 0) {
		std::fclose(fp);
		error = "fichier vide : " + path;
		return false;
	}
	std::vector<uint8_t> buf(static_cast<size_t>(size));
	size_t got = std::fread(buf.data(), 1, buf.size(), fp);
	std::fclose(fp);
	if(got != buf.size()) {
		error = "lecture incomplete de " + path;
		return false;
	}
	return LoadFromBuffer(std::move(buf), error);
}

bool Replay::LoadFromBuffer(std::vector<uint8_t> contents, std::string& error) {
	if(contents.size() < 32) {
		error = "en-tete tronque";
		return false;
	}
	std::memcpy(&id, contents.data() + 0, 4);
	std::memcpy(&version, contents.data() + 4, 4);
	std::memcpy(&flag, contents.data() + 8, 4);
	std::memcpy(&timestamp, contents.data() + 12, 4);
	std::memcpy(&datasize, contents.data() + 16, 4);
	std::memcpy(props, contents.data() + 24, 8);
	size_t header_len = 32;

	if(id != REPLAY_YRP1 && id != REPLAY_YRPX) {
		error = "identifiant de replay inconnu";
		return false;
	}
	if(flag & FLAG_EXTENDED_HEADER) {
		if(contents.size() < 72) {
			error = "en-tete etendu tronque";
			return false;
		}
		std::memcpy(&header_version, contents.data() + 32, 8);
		std::memcpy(seed, contents.data() + 40, 32);
		header_len = 72;
	}

	std::vector<uint8_t> body;
	if(flag & FLAG_COMPRESSED) {
		body.resize(datasize);
		size_t dst_len = datasize;
		SizeT src_len = contents.size() - header_len;
		int rc = LzmaUncompress(body.data(), &dst_len, contents.data() + header_len,
								&src_len, props, 5);
		if(rc != SZ_OK) {
			error = "decompression LZMA en echec (code " + std::to_string(rc) + ")";
			return false;
		}
		body.resize(dst_len);
	} else {
		body.assign(contents.begin() + header_len, contents.end());
	}

	Cursor cur(body);

	// -- noms des joueurs
	if(flag & FLAG_SINGLE_MODE) {
		players.push_back(cur.Name());
		players.push_back(cur.Name());
		home_count = opposing_count = 1;
	} else {
		uint32_t* counts[2] = { &home_count, &opposing_count };
		for(uint32_t* count : counts) {
			if(flag & FLAG_NEWREPLAY)
				*count = cur.Get<uint32_t>();
			else if(flag & FLAG_TAG)
				*count = 2;
			else
				*count = 1;
			for(uint32_t i = 0; i < *count && cur.Ok(); ++i)
				players.push_back(cur.Name());
		}
	}

	// -- parametres de duel
	if(id == REPLAY_YRP1) {
		start_lp = cur.Get<uint32_t>();
		start_hand = cur.Get<uint32_t>();
		draw_count = cur.Get<uint32_t>();
	}
	duel_flags = (flag & FLAG_64BIT_DUELFLAG) ? cur.Get<uint64_t>()
											  : cur.Get<uint32_t>();
	if((flag & FLAG_SINGLE_MODE) && id == REPLAY_YRP1) {
		uint16_t len = cur.Get<uint16_t>();
		scriptname.resize(len);
		if(len)
			cur.Read(&scriptname[0], len);
	}

	if(id == REPLAY_YRP1) {
		// -- decks (le mode hand test conserve les decks, contrairement aux
		//    autres modes solo : replay.cpp:239)
		const bool has_decks = !((flag & FLAG_SINGLE_MODE) && !(flag & FLAG_HAND_TEST));
		if(has_decks) {
			for(uint32_t i = 0; i < home_count + opposing_count && cur.Ok(); ++i) {
				Deck d;
				const uint32_t nm = cur.Get<uint32_t>();
				for(uint32_t j = 0; j < nm && cur.Ok(); ++j)
					d.main.push_back(cur.Get<uint32_t>());
				const uint32_t nx = cur.Get<uint32_t>();
				for(uint32_t j = 0; j < nx && cur.Ok(); ++j)
					d.extra.push_back(cur.Get<uint32_t>());
				// Le compte annonce doit etre servi EN ENTIER. Un corps tronque
				// rendait un deck court sans un mot, et la liste de reponses
				// courte qui suit devient `ref_decisions`, c'est-a-dire le
				// plafond de decisions de toute la recherche : un probleme
				// d'octets se propageait en budget silencieusement reduit (4.5).
				if(d.main.size() != nm || d.extra.size() != nx) {
					error = "replay tronque : deck " + std::to_string(i) +
							" annonce " + std::to_string(nm) + "+" +
							std::to_string(nx) + " cartes, " +
							std::to_string(d.main.size()) + "+" +
							std::to_string(d.extra.size()) + " lues";
					return false;
				}
				decks.push_back(std::move(d));
			}
			if(decks.size() != home_count + opposing_count) {
				error = "replay tronque : " +
						std::to_string(home_count + opposing_count) +
						" deck(s) annonces, " + std::to_string(decks.size()) +
						" lus";
				return false;
			}
			if((flag & FLAG_NEWREPLAY) && !(flag & FLAG_HAND_TEST)) {
				for(uint32_t i = 0, n = cur.Get<uint32_t>(); i < n && cur.Ok(); ++i)
					rule_cards.push_back(cur.Get<uint32_t>());
			}
		}
		// -- reponses du joueur
		while(!cur.Eof()) {
			uint8_t len = cur.Get<uint8_t>();
			// `len == 0` est le terminateur normal ; une lecture qui echoue ne
			// l'est pas.
			if(!cur.Ok()) {
				error = "replay tronque : en-tete de reponse illisible apres " +
						std::to_string(responses.size()) + " reponse(s)";
				return false;
			}
			if(!len)
				break;
			std::vector<uint8_t> r(len);
			if(!cur.Read(r.data(), len)) {
				error = "replay tronque : reponse " +
						std::to_string(responses.size()) + " annoncee a " +
						std::to_string(len) + " octets, corps absent";
				return false;
			}
			responses.push_back(std::move(r));
		}
	} else {
		// -- flux diffuse
		while(!cur.Eof()) {
			uint8_t msg = cur.Get<uint8_t>();
			if(!cur.Ok())
				break;
			uint32_t len = cur.Get<uint32_t>();
			if(!cur.Ok()) {
				error = "replay tronque : longueur du paquet " +
						std::to_string(packets.size()) + " illisible";
				return false;
			}
			std::vector<uint8_t> data(len);
			if(len && !cur.Read(data.data(), len)) {
				error = "replay tronque : paquet " +
						std::to_string(packets.size()) + " annonce a " +
						std::to_string(len) + " octets, corps absent";
				return false;
			}
			if(msg == OLD_REPLAY_MODE) {
				if(!yrp) {
					auto nested = std::make_unique<Replay>();
					std::string nested_error;
					if(nested->LoadFromBuffer(std::move(data), nested_error))
						yrp = std::move(nested);
				}
				continue;
			}
			if(msg == MSG_NEW_TURN)
				++turn_count;
			if(msg == MSG_AI_NAME)
				continue;
			packets.push_back({ msg, std::move(data) });
		}
	}
	return true;
}

bool WriteYrp1(const std::string& path, const Replay& base,
			   const std::vector<std::vector<uint8_t>>& responses,
			   std::string& error) {
	if(base.id != REPLAY_YRP1) {
		error = "le modele n'est pas un yrp1 : decks et parametres manquants";
		return false;
	}
	std::vector<uint8_t> body;
	auto put = [&body](const void* p, size_t n) {
		const auto* b = static_cast<const uint8_t*>(p);
		body.insert(body.end(), b, b + n);
	};
	auto put32 = [&put](uint32_t v) { put(&v, 4); };
	// Noms sur 20 UTF-16 fixes, comme les lit ParseNames (replay.cpp:194).
	auto put_name = [&put](const std::string& s) {
		uint16_t buf[20]{};
		for(size_t i = 0; i < 20 && i < s.size(); ++i)
			buf[i] = static_cast<uint8_t>(s[i]);
		put(buf, sizeof(buf));
	};

	if(base.flag & FLAG_SINGLE_MODE) {
		put_name(base.players.size() > 0 ? base.players[0] : std::string());
		put_name(base.players.size() > 1 ? base.players[1] : std::string());
	} else {
		size_t at = 0;
		for(uint32_t count : { base.home_count, base.opposing_count }) {
			if(base.flag & FLAG_NEWREPLAY)
				put32(count);
			for(uint32_t i = 0; i < count; ++i)
				put_name(at < base.players.size() ? base.players[at++] : std::string());
		}
	}

	put32(base.start_lp);
	put32(base.start_hand);
	put32(base.draw_count);
	if(base.flag & FLAG_64BIT_DUELFLAG) {
		uint64_t v = base.duel_flags;
		put(&v, 8);
	} else {
		put32(static_cast<uint32_t>(base.duel_flags));
	}
	if(base.flag & FLAG_SINGLE_MODE) {
		uint16_t len = static_cast<uint16_t>(base.scriptname.size());
		put(&len, 2);
		put(base.scriptname.data(), len);
	}

	const bool has_decks =
		!((base.flag & FLAG_SINGLE_MODE) && !(base.flag & FLAG_HAND_TEST));
	if(has_decks) {
		for(const Deck& d : base.decks) {
			put32(static_cast<uint32_t>(d.main.size()));
			for(uint32_t c : d.main) put32(c);
			put32(static_cast<uint32_t>(d.extra.size()));
			for(uint32_t c : d.extra) put32(c);
		}
		if((base.flag & FLAG_NEWREPLAY) && !(base.flag & FLAG_HAND_TEST)) {
			put32(static_cast<uint32_t>(base.rule_cards.size()));
			for(uint32_t c : base.rule_cards) put32(c);
		}
	}

	for(const auto& r : responses) {
		if(r.empty() || r.size() > 255) {
			error = "reponse de taille invalide (" + std::to_string(r.size()) + ")";
			return false;
		}
		body.push_back(static_cast<uint8_t>(r.size()));
		put(r.data(), r.size());
	}
	body.push_back(0);   // terminateur lu par ParseResponses

	// En-tete. On ecrit NON COMPRESSE : OpenReplayFromBuffer accepte les deux
	// (replay.cpp:104) et le champ `hash` n'est verifie nulle part, ce qui evite
	// de dependre d'un encodeur LZMA aux memes reglages qu'EDOPro.
	std::vector<uint8_t> out;
	auto head = [&out](const void* p, size_t n) {
		const auto* b = static_cast<const uint8_t*>(p);
		out.insert(out.end(), b, b + n);
	};
	uint32_t flag = (base.flag & ~FLAG_COMPRESSED);
	uint32_t datasize = static_cast<uint32_t>(body.size());
	uint32_t zero = 0;
	head(&base.id, 4);
	head(&base.version, 4);
	head(&flag, 4);
	head(&base.timestamp, 4);
	head(&datasize, 4);
	head(&zero, 4);            // hash : ignore a la lecture
	uint8_t props[8]{};
	head(props, 8);
	if(flag & FLAG_EXTENDED_HEADER) {
		uint64_t hv = base.header_version ? base.header_version : 1;
		head(&hv, 8);
		head(base.seed, 32);
	}
	out.insert(out.end(), body.begin(), body.end());

	FILE* fp = std::fopen(path.c_str(), "wb");
	if(!fp) {
		error = "impossible d'ecrire " + path;
		return false;
	}
	bool ok = std::fwrite(out.data(), 1, out.size(), fp) == out.size();
	std::fclose(fp);
	if(!ok)
		error = "ecriture incomplete de " + path;
	return ok;
}

} // namespace solver
