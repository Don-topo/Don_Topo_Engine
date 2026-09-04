#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace DonTopo {

// Tabla estática de símbolos pa el popup de autocomplete del Script Editor:
// keywords Lua + API expuesta a scripts en ScriptBindings.cpp. Mantenida a
// mano — no se deriva de sol2 por reflexión (fuera de alcance).
const std::vector<std::string>& luaApiSymbols();

// Una sugerencia ya resuelta contra lo que el usuario lleva escrito.
//
// 'insert' y 'replaceOffset' existen porque no siempre se sustituye el
// fragmento entero. Con un receptor conocido ("Transform:Set") se reemplaza
// todo por el símbolo completo; con un receptor que es una VARIABLE LOCAL
// ("t:Set", el caso normal en código real) hay que conservar el "t:" del
// usuario y escribir solo el miembro — insertar el símbolo entero dejaría
// "t:Transform:SetPosition". replaceOffset dice desde qué carácter del
// fragmento empieza la sustitución.
struct LuaApiMatch {
    std::string symbol;      // símbolo completo, lo que se pinta en la lista
    std::string signature;   // "(pos: Vec3)" — vacío en propiedades y constantes
    std::string doc;         // una línea de ayuda; vacía si no la hay
    std::string insert;      // texto que se escribe de verdad
    std::size_t replaceOffset = 0; // desde qué carácter del fragmento se sustituye
};

// Sugerencias para el fragmento bajo el cursor, ya ordenadas: primero las que
// empiezan por el fragmento entero, después las que solo casan por el nombre
// del miembro; a igualdad, la más corta, y a igualdad de longitud, alfabético
// (orden total y determinista, para que sea comprobable en un test).
//
// Fragmento vacío -> lista vacía: el popup no debe abrirse sin nada escrito.
// maxResults == 0 significa sin límite.
std::vector<LuaApiMatch> luaApiMatches(const std::string& fragment, std::size_t maxResults = 0);

// Firma y documentación de un símbolo concreto, vacías si no están anotadas.
// Separado de la lista de símbolos a propósito: la lista es la autoridad sobre
// QUÉ existe (y la que hay que tocar al añadir un binding), esto es solo el
// texto de ayuda, que puede faltar sin que el símbolo desaparezca del popup.
void luaApiDoc(const std::string& symbol, std::string& outSignature, std::string& outDoc);

// Entradas dinámicas del autocomplete: los snippets de las acciones del panel
// Input Actions (Input.IsActionDown("Jump")...). Las publica el editor cada vez
// que se crea, renombra o borra una acción; se concatenan a la tabla estática.
void setLuaApiActionSymbols(std::vector<std::string> symbols);

} // namespace DonTopo
