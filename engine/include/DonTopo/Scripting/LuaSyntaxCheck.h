#pragma once
#include <optional>
#include <string>
#include <utility>

namespace DonTopo {

// Compila (no ejecuta) source en un lua_State descartable, cerrado siempre
// antes de retornar. nullopt si compila sin error. Si falla: {línea,
// mensaje} parseados del formato de error de Lua
// ([string "..."]:LINE: mensaje).
std::optional<std::pair<int, std::string>> checkLuaSyntax(const std::string& source);

} // namespace DonTopo
