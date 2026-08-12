// Lecture des replays EDOPro (.yrpX et .yrp1 embarque).
//
// Version allegee de edopro/gframe/replay.cpp : on ne garde que ce dont le
// solveur a besoin (decks, seed, parametres de duel, reponses du joueur) et on
// se passe de toute dependance a Irrlicht / au reste de gframe.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace solver {

inline constexpr uint32_t REPLAY_YRP1 = 0x31707279;
inline constexpr uint32_t REPLAY_YRPX = 0x58707279;

inline constexpr uint32_t FLAG_COMPRESSED      = 0x001;
inline constexpr uint32_t FLAG_TAG             = 0x002;
inline constexpr uint32_t FLAG_SINGLE_MODE     = 0x008;
inline constexpr uint32_t FLAG_NEWREPLAY       = 0x020;
inline constexpr uint32_t FLAG_HAND_TEST       = 0x040;
inline constexpr uint32_t FLAG_64BIT_DUELFLAG  = 0x100;
inline constexpr uint32_t FLAG_EXTENDED_HEADER = 0x200;

struct Deck {
	std::vector<uint32_t> main;
	std::vector<uint32_t> extra;
};

// Un paquet du flux diffuse (yrpX) : le type de message et sa charge utile.
struct Packet {
	uint8_t message{};
	std::vector<uint8_t> data;
};

class Replay {
public:
	// Renvoie false et remplit `error` si le fichier est illisible.
	bool Load(const std::string& path, std::string& error);
	bool LoadFromBuffer(std::vector<uint8_t> contents, std::string& error);

	bool IsStreamed() const { return id == REPLAY_YRPX; }
	bool IsHandTest() const { return (flag & FLAG_HAND_TEST) != 0; }

	// Le yrp1 embarque porte les reponses du joueur ; sans lui, aucune
	// reconstitution n'est possible (cf. docs/combo-solver-design.md 2.2).
	const Replay* Embedded() const { return yrp.get(); }

	uint32_t id{}, version{}, flag{}, timestamp{}, datasize{};
	uint64_t header_version{};
	uint64_t seed[4]{};
	uint8_t props[8]{};

	uint32_t start_lp{}, start_hand{}, draw_count{};
	uint64_t duel_flags{};
	std::string scriptname;

	uint32_t home_count{}, opposing_count{};
	std::vector<std::string> players;
	std::vector<Deck> decks;
	std::vector<uint32_t> rule_cards;

	std::vector<std::vector<uint8_t>> responses;
	std::vector<Packet> packets;
	int turn_count{};

private:
	std::unique_ptr<Replay> yrp;
};

// Ecrit un replay rejouable par EDOPro : meme position de depart que `base`
// (noms, parametres, graine, decks) mais avec les reponses fournies.
//
// `base` doit etre un yrp1 — c'est lui qui porte decks et parametres ; le yrpX
// qui l'enveloppe n'est qu'un flux de diffusion, non rejouable seul.
bool WriteYrp1(const std::string& path, const Replay& base,
			   const std::vector<std::vector<uint8_t>>& responses,
			   std::string& error);

} // namespace solver
