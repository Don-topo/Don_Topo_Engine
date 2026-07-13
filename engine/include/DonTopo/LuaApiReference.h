#pragma once
#include <string>
#include <vector>

namespace DonTopo {

// Tabla estática de símbolos pa el popup de autocomplete del Script Editor:
// keywords Lua + API expuesta a scripts en ScriptBindings.cpp. Mantenida a
// mano — no se deriva de sol2 por reflexión (fuera de alcance).
const std::vector<std::string>& luaApiSymbols();

} // namespace DonTopo
