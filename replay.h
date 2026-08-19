// Reading EDOPro replays (.yrpX, and the .yrp1 embedded inside it).
//
// Trimmed-down version of edopro/gframe/replay.cpp: only what the solver needs
// (decks, seed, duel parameters, player answers), with no dependency on
// Irrlicht or on the rest of gframe.
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

// One packet of the broadcast stream (yrpX): message type and payload.
struct Packet {
	uint8_t message{};
	std::vector<uint8_t> data;
};

class Replay {
public:
	// Returns false and fills `error` when the file cannot be read.
	bool Load(const std::string& path, std::string& error);
	bool LoadFromBuffer(std::vector<uint8_t> contents, std::string& error);

	bool IsStreamed() const { return id == REPLAY_YRPX; }
	bool IsHandTest() const { return (flag & FLAG_HAND_TEST) != 0; }

	// The embedded yrp1 carries the player answers; without it nothing can be
	// reconstructed.
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

// Writes a replay EDOPro can play back: same starting position as `base`
// (names, parameters, seed, decks) but with the answers supplied here.
//
// `base` must be a yrp1 -- that is what carries the decks and the parameters;
// the yrpX wrapping it is only a broadcast stream, not replayable on its own.
bool WriteYrp1(const std::string& path, const Replay& base,
			   const std::vector<std::vector<uint8_t>>& responses,
			   std::string& error);

} // namespace solver
