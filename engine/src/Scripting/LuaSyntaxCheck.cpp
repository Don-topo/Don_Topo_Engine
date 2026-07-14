#include "DonTopo/Scripting/LuaSyntaxCheck.h"
#include <regex>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace DonTopo {

std::optional<std::pair<int, std::string>> checkLuaSyntax(const std::string& source)
{
    lua_State* L = luaL_newstate();
    if (!L)
        return std::nullopt;

    int status = luaL_loadstring(L, source.c_str());
    if (status == LUA_OK)
    {
        lua_close(L);
        return std::nullopt;
    }

    const char* raw = lua_tostring(L, -1);
    std::string message = raw ? raw : "error de sintaxis desconocido";
    lua_close(L);

    static const std::regex linePattern(R"(:(\d+):\s*(.*))");
    std::smatch match;
    if (std::regex_search(message, match, linePattern))
        return std::make_pair(std::stoi(match[1].str()), match[2].str());

    return std::make_pair(1, message);
}

} // namespace DonTopo
