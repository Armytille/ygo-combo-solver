// Decoding of the MSG_SELECT_* messages: the solver's branch points.
//
// Every message already carries the list of legal choices, so no game rule has
// to be reimplemented here.
#pragma once

#include <cstdint>
#include <string>

namespace solver {

// -1 when the message is unrecognised or truncated.
struct PromptInfo {
	uint8_t type{};
	int player{ -1 };
	long double raw{ -1 };    // number of distinct answers offered
	long double dedup{ -1 };  // after merging choices that share a code
	std::string detail;
};

bool IsPrompt(uint8_t message);
const char* PromptName(uint8_t message);
PromptInfo DecodePrompt(uint8_t message, const uint8_t* data, uint32_t len);

} // namespace solver
