#pragma once
#include <optional>
#include <string>
#include <utility>

namespace DonTopo {

// Compiles (does not run) source in a throwaway lua_State, always closed
// before returning. nullopt if it compiles without error. On failure: {line,
// message} parsed from Lua's error format
// ([string "..."]:LINE: message).
std::optional<std::pair<int, std::string>> checkLuaSyntax(const std::string& source);

} // namespace DonTopo
