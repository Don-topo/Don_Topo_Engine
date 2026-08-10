#pragma once
#include <string>
#include <vector>

namespace DonTopo {

// Tabla estática de símbolos pa el popup de autocomplete del Script Editor:
// keywords Lua + API expuesta a scripts en ScriptBindings.cpp. Mantenida a
// mano — no se deriva de sol2 por reflexión (fuera de alcance).
const std::vector<std::string>& luaApiSymbols();

// Entradas dinámicas del autocomplete: los snippets de las acciones del panel
// Input Actions (Input.IsActionDown("Jump")...). Las publica el editor cada vez
// que se crea, renombra o borra una acción; se concatenan a la tabla estática.
void setLuaApiActionSymbols(std::vector<std::string> symbols);

} // namespace DonTopo
