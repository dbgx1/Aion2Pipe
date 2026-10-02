#pragma once
#include "game_protocol.hpp"
namespace aion {
// Conservative plaintext recognition independent of transport ports. The caller
// keeps the original stream and starts its model at the returned frame boundary.
std::optional<size_t> findWorldStreamStart(std::span<const uint8_t> bytes);
}
