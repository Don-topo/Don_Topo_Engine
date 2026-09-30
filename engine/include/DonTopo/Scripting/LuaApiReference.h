#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace DonTopo {

// Static symbol table for the Script Editor's autocomplete popup:
// Lua keywords + API exposed to scripts in ScriptBindings.cpp. Maintained by
// hand; it is not derived from sol2 by reflection (out of scope).
const std::vector<std::string>& luaApiSymbols();

// A suggestion already resolved against what the user has typed.
//
// 'insert' and 'replaceOffset' exist because the whole fragment is not always
// replaced. With a known receiver ("Transform:Set") everything is replaced
// by the full symbol; with a receiver that is a LOCAL VARIABLE
// ("t:Set", the normal case in real code) the user's "t:" has to be kept
// and only the member written: inserting the whole symbol would leave
// "t:Transform:SetPosition". replaceOffset says from which character of the
// fragment the replacement starts.
struct LuaApiMatch {
    std::string symbol;      // full symbol, what is drawn in the list
    std::string signature;   // "(pos: Vec3)"; empty for properties and constants
    std::string doc;         // one line of help; empty if there is none
    std::string insert;      // text that is actually written
    std::size_t replaceOffset = 0; // from which character of the fragment it is replaced
};

// Suggestions for the fragment under the cursor, already sorted: first those that
// start with the whole fragment, then those that only match by member
// name; on a tie, the shortest, and on equal length, alphabetical
// (a total, deterministic order, so it can be checked in a test).
//
// Empty fragment -> empty list: the popup must not open with nothing typed.
// maxResults == 0 means no limit.
std::vector<LuaApiMatch> luaApiMatches(const std::string& fragment, std::size_t maxResults = 0);

// Signature and documentation of a specific symbol, empty if not annotated.
// Kept apart from the symbol list on purpose: the list is the authority on
// WHAT exists (and the one to touch when adding a binding), this is only the
// help text, which can be missing without the symbol disappearing from the popup.
void luaApiDoc(const std::string& symbol, std::string& outSignature, std::string& outDoc);

// Dynamic autocomplete entries: the snippets of the actions in the
// Input Actions panel (Input.IsActionDown("Jump")...). The editor publishes them every time
// an action is created, renamed or deleted; they are concatenated to the static table.
void setLuaApiActionSymbols(std::vector<std::string> symbols);

} // namespace DonTopo
