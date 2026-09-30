#include "DonTopo/Editor/ScriptEditorPanel.h"
#include "DonTopo/Files/FileManager.h"
#include "DonTopo/Scripting/LuaSyntaxCheck.h"
#include "DonTopo/Scripting/LuaApiReference.h"
#include <imgui.h>
#include <optional>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <regex>
#include <system_error>

namespace DonTopo {

namespace {

// Calm frames before rechecking the syntax. At 60 fps that is ~0.2 s: just
// enough not to fire in the middle of a half-typed word and short
// enough for the error to show up on its own, without having to save.
constexpr int kSyntaxDelayFrames = 12;

// Left margin of the line-number gutter of the vendored widget
// (TextEditor.cpp, mLeftMargin from the constructor). It is private and there is no getter;
// it is replicated here, just as its column<->index conversion is replicated.
constexpr float kTextEditorLeftMargin = 10.0f;

bool isFragmentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':';
}

// GetCurrentLineText()/GetCursorPosition().mColumn live in different spaces:
// the first returns the real characters of the line (a literal '\t' takes up
// a single position), whereas mColumn is a *visual* column (a '\t'
// counts as up to GetTabSize() cells, see TextEditor.h, doc of Coordinates).
// Indexing the line with mColumn directly is incorrect on lines with preceding
// tabs. TextEditor::GetCharacterIndex/GetCharacterColumn do this
// conversion but are private in the vendored widget (TextEditor.h line
// 332-333), so we replicate the same algorithm here (TextEditor.cpp
// lines 492-527) over the public std::string we already have.
int utf8CharLength(unsigned char c)
{
    if ((c & 0xFE) == 0xFC) return 6;
    if ((c & 0xFC) == 0xF8) return 5;
    if ((c & 0xF8) == 0xF0) return 4;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xE0) == 0xC0) return 2;
    return 1;
}

// Visual column -> real character index (equivalent to GetCharacterIndex).
int characterIndexFromColumn(const TextEditor& editor, const std::string& line, int column)
{
    int tabSize = editor.GetTabSize();
    int c = 0;
    int i = 0;
    for (; i < static_cast<int>(line.size()) && c < column;)
    {
        if (line[i] == '\t')
            c = (c / tabSize) * tabSize + tabSize;
        else
            ++c;
        i += utf8CharLength(static_cast<unsigned char>(line[i]));
    }
    return i;
}

// Real character index -> visual column (equivalent to GetCharacterColumn).
int characterColumnFromIndex(const TextEditor& editor, const std::string& line, int index)
{
    int tabSize = editor.GetTabSize();
    int col = 0;
    int i = 0;
    while (i < index && i < static_cast<int>(line.size()))
    {
        char c = line[i];
        i += utf8CharLength(static_cast<unsigned char>(c));
        if (c == '\t')
            col = (col / tabSize) * tabSize + tabSize;
        else
            ++col;
    }
    return col;
}

// Scans GetCurrentLineText() backwards from the cursor column
// while the characters are part of an identifier/dotted path
// (supports "Entity:Get...", "Log.I..."). Returns the fragment and its start column
// (real character index, not visual) on the same line as the
// cursor.
struct Fragment { std::string text; int startColumn; };

// Line where to draw the marker of a syntax error, given the error
// returned by checkLuaSyntax and how many lines the editor has.
//
// There are two traps, both measured (see the tests in scripting_tests.cpp):
//
// 1. Lua reports the "something left unclosed" errors at <eof>, which falls ONE LINE
//    PAST the end of the document, and the editor only draws markers for
//    lines that exist, so that marker was never drawn. It is the most
//    frequent case: it is what happens when deleting an 'end'.
// 2. Simply clamping it to the last line is not much use either: the editor
//    adds a trailing newline, so that last line is usually EMPTY and the
//    red band ends up at the end of the file, where it says nothing.
//
// That is why, when Lua names the construct that was left open ("'end'
// expected (to close 'function' at line 12)"), THAT line is marked: it is where
// the real problem is. If it does not name it, it falls back to the clamp.
int markerLine(const std::pair<int, std::string>& err, int totalLines)
{
    static const std::regex openedAt(R"(to close '[^']*' at line (\d+))");
    std::smatch match;
    if (std::regex_search(err.second, match, openedAt))
    {
        const int opened = std::stoi(match[1].str());
        if (opened >= 1 && opened <= totalLines) return opened;
    }
    const int line = (err.first > totalLines) ? totalLines : err.first;
    return line < 1 ? 1 : line;
}

Fragment extractFragment(const TextEditor& editor)
{
    TextEditor::Coordinates cursor = editor.GetCursorPosition();
    std::string line = editor.GetCurrentLineText();
    int col = std::min(characterIndexFromColumn(editor, line, cursor.mColumn),
        static_cast<int>(line.size()));

    int start = col;
    while (start > 0 && isFragmentChar(line[start - 1]))
        --start;

    return Fragment{ line.substr(start, col - start), start };
}

} // anonymous namespace

void ScriptEditorPanel::openFile(const std::filesystem::path& path)
{
    m_open = true;
    // Opening a file is an explicit request to look at it: besides existing,
    // the window has to come to the front. It is requested here and consumed in draw()
    // because SetNextWindowFocus only works right before the window's Begin.
    m_focusWindowRequested = true;

    // We canonicalize the path before comparing/storing: the different call sites
    // (Content Browser vs Properties/New-Script) build the same real file
    // from different roots, and a lexical comparison may not match (".." ,
    // separators, drive letter case, etc.), leading to duplicate tabs that
    // silently overwrite each other's changes on save.
    std::error_code ec;
    std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, ec);
    if (ec) canonicalPath = path;

    for (size_t i = 0; i < m_tabs.size(); ++i)
    {
        if (m_tabs[i].path == canonicalPath)
        {
            m_focusIndex = static_cast<int>(i);
            return;
        }
    }

    std::optional<std::string> content = FileManager::readText(path.string());
    if (!content)
    {
        log("Script Editor: could not open '" + path.string() + "'");
        return;
    }

    Tab tab;
    tab.path = canonicalPath;
    tab.editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    tab.editor.SetText(*content);
    // Starting point to detect foreign changes: if the mtime moves without
    // us having saved, the file was touched by someone else.
    std::error_code timeEc;
    tab.diskTime = std::filesystem::last_write_time(canonicalPath, timeEc);
    // Entry diagnostic: a file that already comes broken from disk must
    // show the error when opened, not wait for the first Ctrl+S.
    refreshDiagnostics(tab);
    m_tabs.push_back(std::move(tab));
    m_focusIndex = static_cast<int>(m_tabs.size()) - 1;
}

void ScriptEditorPanel::applyMatch(Tab& tab, const LuaApiMatch& match)
{
    // DeleteRange/InsertTextAt are private in the vendored TextEditor (see
    // TextEditor.h line 325), so the equivalent public API is used: select
    // the range to replace and Delete(). Delete() does not no-op if start == end
    // (unlike DeleteRange), so it only deletes when there is something.
    //
    // The start of the substitution is NOT always that of the fragment: a
    // suggestion found by member name ("t:Get" -> GetTransform)
    // keeps the "t:" the user typed. The characters of the fragment
    // are alphanumerics, '_', '.' and ':' (never tabs), so the
    // displacement in characters and in visual columns coincides.
    const TextEditor::Coordinates start(
        tab.acFragmentStart.mLine,
        tab.acFragmentStart.mColumn + static_cast<int>(match.replaceOffset));

    TextEditor::Coordinates cursor = tab.editor.GetCursorPosition();
    if (cursor != start)
    {
        tab.editor.SetSelection(start, cursor);
        tab.editor.Delete();
    }
    tab.editor.SetCursorPosition(start);
    tab.editor.InsertText(match.insert);
    tab.dirty = true;
    tab.acVisible = false;
}

void ScriptEditorPanel::refreshDiagnostics(Tab& tab)
{
    // The syntax check is shown via visual marker and status bar,
    // never to the Log Console: it would be noise redundant with the marker.
    TextEditor::ErrorMarkers markers;
    auto err = checkLuaSyntax(tab.editor.GetText());
    if (err)
    {
        const int line = markerLine(*err, tab.editor.GetTotalLines());
        markers[line] = err->second;
        tab.errorLine = line;
        tab.errorMessage = err->second;
    }
    else
    {
        tab.errorLine = 0;
        tab.errorMessage.clear();
    }
    tab.editor.SetErrorMarkers(markers);
    tab.syntaxDelay = -1;
}

void ScriptEditorPanel::reloadFromDisk(Tab& tab)
{
    std::optional<std::string> content = FileManager::readText(tab.path.string());
    if (!content)
    {
        log("Script Editor: could not re-read '" + tab.path.string() + "'");
        return;
    }
    tab.editor.SetText(*content);
    tab.dirty = false;
    tab.externalChange = false;
    std::error_code ec;
    tab.diskTime = std::filesystem::last_write_time(tab.path, ec);
    refreshDiagnostics(tab);
}

void ScriptEditorPanel::saveTab(Tab& tab)
{
    if (FileManager::writeText(tab.path.string(), tab.editor.GetText()))
    {
        tab.dirty = false;
        // The new mtime was caused by us: it is noted so as not to
        // confuse it with a foreign edit on the next frame.
        std::error_code ec;
        tab.diskTime = std::filesystem::last_write_time(tab.path, ec);
        tab.externalChange = false;
    }
    else
        log("Script Editor: could not save '" + tab.path.string() + "'");

    refreshDiagnostics(tab);
}

// Searches forward or backward from the cursor, wrapping around at the
// opposite end. It works over GetTextLines() instead of GetText()
// because the result has to be expressed as (line, column) and splitting
// a plain text by line breaks again would mean walking it twice.
bool ScriptEditorPanel::findNext(Tab& tab, bool backwards)
{
    const std::string needle(tab.findBuffer);
    if (needle.empty()) return false;

    std::vector<std::string> lines = tab.editor.GetTextLines();
    if (lines.empty()) return false;

    auto normalize = [&tab](std::string s) {
        if (!tab.findCaseSensitive)
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string target = normalize(needle);

    const TextEditor::Coordinates cursor = tab.editor.GetCursorPosition();
    const int total = static_cast<int>(lines.size());
    const int startLine = std::min(std::max(cursor.mLine, 0), total - 1);

    // Walk of 'total' lines starting from the cursor's: the first
    // pass starts from the cursor column and the others from the edge.
    for (int step = 0; step <= total; ++step)
    {
        const int lineNo = backwards
            ? ((startLine - step) % total + total) % total
            : (startLine + step) % total;
        const std::string haystack = normalize(lines[lineNo]);

        std::size_t found = std::string::npos;
        if (step == 0)
        {
            // On the cursor's line only what remains ahead (or behind) counts:
            // otherwise, every F3 would return the same match.
            const int col = std::min(
                characterIndexFromColumn(tab.editor, lines[lineNo], cursor.mColumn),
                static_cast<int>(haystack.size()));
            if (backwards)
            {
                if (col > 0) found = haystack.rfind(target, static_cast<std::size_t>(col) - 1);
            }
            else
                found = haystack.find(target, static_cast<std::size_t>(col));
        }
        else
            found = backwards ? haystack.rfind(target) : haystack.find(target);

        if (found == std::string::npos) continue;

        const int beginCol = characterColumnFromIndex(tab.editor, lines[lineNo],
            static_cast<int>(found));
        const int endCol = characterColumnFromIndex(tab.editor, lines[lineNo],
            static_cast<int>(found + target.size()));
        tab.editor.SetCursorPosition(TextEditor::Coordinates(lineNo, endCol));
        tab.editor.SetSelection(TextEditor::Coordinates(lineNo, beginCol),
                                TextEditor::Coordinates(lineNo, endCol));
        tab.findStatus.clear();
        return true;
    }

    tab.findStatus = "no matches";
    return false;
}

// Find/replace bar and go-to-line. Returns true if it has consumed the
// keyboard this frame: while focus is on one of its fields, the editor must
// not process the same key.
bool ScriptEditorPanel::drawFindBar(Tab& tab)
{
    bool consumed = false;

    if (tab.gotoOpen)
    {
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputInt("Linea", &tab.gotoLine, 0, 0,
                            ImGuiInputTextFlags_EnterReturnsTrue))
        {
            const int total = tab.editor.GetTotalLines();
            const int target = std::min(std::max(tab.gotoLine, 1), total);
            // Coordinates is 0-based and what the user types is 1-based.
            tab.editor.SetCursorPosition(TextEditor::Coordinates(target - 1, 0));
            tab.gotoOpen = false;
        }
        if (ImGui::IsItemActive()) consumed = true;
        ImGui::SameLine();
        if (ImGui::Button("Close##goto")) tab.gotoOpen = false;
    }

    if (!tab.findOpen) return consumed;

    if (tab.findFocusRequested)
    {
        ImGui::SetKeyboardFocusHere();
        tab.findFocusRequested = false;
    }
    ImGui::SetNextItemWidth(180.0f);
    const bool submitted = ImGui::InputText("##buscar", tab.findBuffer, sizeof(tab.findBuffer),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
    const bool findFieldActive = ImGui::IsItemActive();
    if (submitted) findNext(tab, false);
    ImGui::SameLine();
    ImGui::TextUnformatted("Find");

    ImGui::SameLine();
    ImGui::Checkbox("Aa", &tab.findCaseSensitive);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Match case");

    ImGui::SameLine();
    if (ImGui::Button("<")) findNext(tab, true);
    ImGui::SameLine();
    if (ImGui::Button(">")) findNext(tab, false);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("##reemplazar", tab.replaceBuffer, sizeof(tab.replaceBuffer));
    const bool replaceFieldActive = ImGui::IsItemActive();
    ImGui::SameLine();
    ImGui::TextUnformatted("Replace with");

    ImGui::SameLine();
    if (ImGui::Button("Replace"))
    {
        // It only replaces if what is selected IS the match: pressing
        // Replace without having searched before would search and replace in one go,
        // which is not what anyone expects from the first click.
        const std::string selected = tab.editor.GetSelectedText();
        const std::string needle(tab.findBuffer);
        auto sameText = [&tab](std::string a, std::string b) {
            if (!tab.findCaseSensitive)
            {
                auto lower = [](std::string& s) {
                    std::transform(s.begin(), s.end(), s.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                };
                lower(a); lower(b);
            }
            return a == b;
        };
        if (!needle.empty() && !selected.empty() && sameText(selected, needle))
        {
            tab.editor.Delete();
            tab.editor.InsertText(tab.replaceBuffer);
            tab.dirty = true;
            tab.syntaxDelay = kSyntaxDelayFrames;
        }
        findNext(tab, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("All"))
    {
        const std::string needle(tab.findBuffer);
        if (!needle.empty())
        {
            // Over the whole text at once: going match by match
            // with the cursor forces keeping count of how much everything behind has moved
            // each time the replacement changes length.
            std::string text = tab.editor.GetText();
            const std::string replacement(tab.replaceBuffer);
            std::string result;
            int count = 0;
            std::size_t pos = 0;
            auto foldCase = [&tab](const std::string& s) {
                if (tab.findCaseSensitive) return s;
                std::string out = s;
                std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return out;
            };
            const std::string hay = foldCase(text);
            const std::string pin = foldCase(needle);
            while (true)
            {
                const std::size_t hit = hay.find(pin, pos);
                if (hit == std::string::npos) break;
                result.append(text, pos, hit - pos);
                result += replacement;
                pos = hit + pin.size();
                ++count;
            }
            if (count > 0)
            {
                result.append(text, pos, std::string::npos);
                tab.editor.SetText(result);
                tab.dirty = true;
                tab.syntaxDelay = kSyntaxDelayFrames;
            }
            tab.findStatus = count > 0 ? (std::to_string(count) + " replacements")
                                       : std::string("no matches");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close##buscar"))
    {
        tab.findOpen = false;
        tab.findStatus.clear();
    }
    if (!tab.findStatus.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tab.findStatus.c_str());
    }

    // Escape closes the bar, but only if focus is on it: otherwise it
    // would steal the Escape that dismisses the autocomplete popup.
    if ((findFieldActive || replaceFieldActive) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        tab.findOpen = false;
        tab.findStatus.clear();
    }
    return consumed || findFieldActive || replaceFieldActive;
}

void ScriptEditorPanel::drawStatusBar(Tab& tab)
{
    const TextEditor::Coordinates cursor = tab.editor.GetCursorPosition();
    ImGui::Separator();
    // Line and column 1-based, as Lua itself counts them when reporting an
    // error: 0-based the bar's number and the error's would not match.
    ImGui::Text("Ln %d, Col %d  |  %d lineas", cursor.mLine + 1, cursor.mColumn + 1,
                tab.editor.GetTotalLines());
    ImGui::SameLine();
    if (tab.errorLine > 0)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "|  Linea %d: %s",
                           tab.errorLine, tab.errorMessage.c_str());
        // A click takes you to the error: the marker is in the gutter and with a
        // long file it can be off screen.
        if (ImGui::IsItemClicked())
            tab.editor.SetCursorPosition(TextEditor::Coordinates(tab.errorLine - 1, 0));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click to go to the error line");
    }
    else
        ImGui::TextDisabled("|  no syntax errors");
}

void ScriptEditorPanel::draw()
{
    if (!m_open) return;
    if (m_focusWindowRequested)
    {
        ImGui::SetNextWindowFocus();
        m_focusWindowRequested = false;
    }
    ImGui::Begin("Script Editor", &m_open);

    int closeRequested = -1;

    if (ImGui::BeginTabBar("##ScriptEditorTabs", ImGuiTabBarFlags_Reorderable))
    {
        for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        {
            Tab& tab = m_tabs[i];
            // The TabItem label must never change its text: even though the ID is
            // stable (the "##" + path below), ImGui loses focus of the child inside
            // (the editor) as soon as a tab's VISIBLE TEXT changes between
            // frames, confirmed by bisecting (appending " *" to the title when going dirty
            // caused loss of the editor's focus one frame later, with or without a stable
            // ID). That is why the "unsaved" state is indicated with the native flag
            // ImGuiTabItemFlags_UnsavedDocument (a dot next to the label) instead of
            // touching the text.
            std::string title = tab.path.filename().string();
            // "##" + path: the TabItem's ID is independent of the visible text,
            // so reordering tabs or duplicate paths in different folders do not
            // collide.
            std::string tabLabel = title + "##" + tab.path.string();
            ImGuiTabItemFlags flags = (m_focusIndex == i) ? ImGuiTabItemFlags_SetSelected
                                                           : ImGuiTabItemFlags_None;
            if (tab.dirty)
                flags |= ImGuiTabItemFlags_UnsavedDocument;
            bool open = true;

            ImGui::PushID(i);
            if (ImGui::BeginTabItem(tabLabel.c_str(), &open, flags))
            {
                if (ImGui::Button("Save"))
                    saveTab(tab);
                ImGui::SameLine();
                if (ImGui::Button("Find"))
                {
                    tab.findOpen = true;
                    tab.findFocusRequested = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Ir a linea"))
                    tab.gotoOpen = true;
                ImGui::SameLine();
                if (ImGui::Button("Reload"))
                    reloadFromDisk(tab);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Re-reads the file from disk and discards unsaved changes");

                const bool panelFocused =
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
                if (panelFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
                    saveTab(tab);
                if (panelFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
                {
                    tab.findOpen = true;
                    tab.findFocusRequested = true;
                }
                if (panelFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false))
                    tab.gotoOpen = true;
                // F3 / Shift+F3 repeat the search without going back to the bar.
                if (panelFocused && ImGui::IsKeyPressed(ImGuiKey_F3, false))
                    findNext(tab, ImGui::GetIO().KeyShift);

                // Foreign change on disk: detected by the mtime. If the tab has
                // no changes of its own it reloads by itself (it is what the user would
                // want, and this way the text does not lie about what is on disk);
                // if it does, the user is asked, because either of the two
                // options loses someone's work.
                {
                    std::error_code ec;
                    const auto now = std::filesystem::last_write_time(tab.path, ec);
                    if (!ec && now != tab.diskTime)
                    {
                        tab.diskTime = now;
                        if (tab.dirty)
                            tab.externalChange = true;
                        else
                        {
                            reloadFromDisk(tab);
                            log("Script Editor: '" + tab.path.filename().string() +
                                "' changed on disk and was reloaded");
                        }
                    }
                }
                if (tab.externalChange)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                        "This file changed on disk and you have unsaved changes.");
                    ImGui::SameLine();
                    if (ImGui::Button("Reload and lose mine"))
                        reloadFromDisk(tab);
                    ImGui::SameLine();
                    if (ImGui::Button("Keep mine"))
                        tab.externalChange = false;
                }

                const bool findConsumed = drawFindBar(tab);

                bool acKeyConsumed = findConsumed;
                if (!acKeyConsumed && tab.acVisible &&
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                {
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
                    {
                        tab.acSelected = (tab.acSelected + 1) % static_cast<int>(tab.acMatches.size());
                        acKeyConsumed = true;
                    }
                    else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
                    {
                        tab.acSelected = (tab.acSelected - 1 + static_cast<int>(tab.acMatches.size())) %
                                         static_cast<int>(tab.acMatches.size());
                        acKeyConsumed = true;
                    }
                    else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Tab, false))
                    {
                        // DeleteRange/InsertTextAt are private in the vendored TextEditor
                        // (see TextEditor.h line 325), so we use the equivalent public API:
                        // select the fragment's range and Delete(). Delete() does not no-op
                        // if start==end (unlike DeleteRange), so we only
                        // select/delete when there is something real to delete.
                        applyMatch(tab, tab.acMatches[tab.acSelected]);
                        acKeyConsumed = true;
                    }
                    else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                    {
                        tab.acVisible = false;
                        tab.acDismissed = true;
                        tab.acDismissedFragment = tab.acLastFragment;
                        acKeyConsumed = true;
                    }
                }
                // Ctrl+Space forces the popup to open even without a previous
                // acVisible. It has to be detected here, before Render(), and added
                // to the disabling of the editor's keyboard handling;
                // detecting it after Render() (as it was) let the editor's
                // HandleKeyboardInputs() already process the key in that same frame
                // and insert a literal space.
                bool forceOpen = !acKeyConsumed &&
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false);

                // We only disable the editor's keyboard handling on the frame in
                // which we really consume one of the popup's keys (or
                // force its opening); on the rest of the frames with the popup
                // open, typing/moving the caret with arrows keeps
                // working normally.
                // (SetHandleKeyboardInputs(false) affects the *next*
                // Render(), which is why this block runs before the Render() below:
                // this way the frame in which a key is consumed is the same
                // frame in which handling is disabled before the editor
                // processes it.)
                bool suppressEditorInput = acKeyConsumed || forceOpen;
                tab.editor.SetHandleKeyboardInputs(!suppressEditorInput);

                // TextEditor::Render() only sets io.WantCaptureKeyboard = true
                // inside its own HandleKeyboardInputs(), which we skip
                // above on purpose. Without this, the Enter/Tab/arrows we have just
                // consumed for the popup are left free for ImGui's Nav system, which
                // uses them to move keyboard focus to another widget
                // (e.g. it switches tab on the tab bar, or leaves the editor without focus,
                // the current line is drawn in gray). We claim the capture
                // ourselves so that Nav does not touch that same key.
                if (suppressEditorInput)
                    ImGui::GetIO().WantCaptureKeyboard = true;

                // The editor takes what remains minus the status bar, which goes
                // below and is always visible: without reserving it, Render() eats
                // the whole height and the bar ends up outside the panel.
                const float statusHeight = ImGui::GetTextLineHeightWithSpacing() +
                    ImGui::GetStyle().ItemSpacing.y;
                ImVec2 editorSize = ImGui::GetContentRegionAvail();
                editorSize.y = std::max(editorSize.y - statusHeight, statusHeight);
                tab.editor.Render("##TextEditor", editorSize);
                if (tab.editor.IsTextChanged())
                {
                    tab.dirty = true;
                    // The countdown is re-armed on every keypress: the analysis
                    // runs when the user stops, not while they type.
                    tab.syntaxDelay = kSyntaxDelayFrames;
                }
                if (tab.syntaxDelay > 0)
                    --tab.syntaxDelay;
                else if (tab.syntaxDelay == 0)
                    refreshDiagnostics(tab);   // leaves syntaxDelay at -1

                ImVec2 editorOrigin = ImGui::GetItemRectMin();
                ImVec2 editorEnd    = ImGui::GetItemRectMax();

                TextEditor::Coordinates currentCursor = tab.editor.GetCursorPosition();
                bool cursorMoved = !acKeyConsumed && (currentCursor != tab.acLastCursor);
                tab.acLastCursor = currentCursor;
                if (cursorMoved && tab.acVisible)
                    tab.acVisible = false;

                Fragment frag = extractFragment(tab.editor);
                tab.acLastFragment = frag.text;
                if (tab.acDismissed && !frag.text.starts_with(tab.acDismissedFragment))
                    tab.acDismissed = false;

                // A freshly typed '.' or ':' opens the popup even if the
                // fragment does not reach the minimum number of characters: typing the
                // separator is exactly the moment when one wants to see what is
                // inside the receiver.
                const bool afterSeparator = !frag.text.empty() &&
                    (frag.text.back() == '.' || frag.text.back() == ':');
                if (!acKeyConsumed &&
                    (forceOpen || (tab.editor.IsTextChanged() && !tab.acDismissed &&
                                   (frag.text.size() >= 2 || afterSeparator))))
                {
                    // The filter lives in LuaApiReference (Core): besides the
                    // prefix of the whole symbol, it matches by MEMBER name,
                    // which is what is needed when the receiver is a local
                    // variable ("t:Get") and not the name of a type.
                    tab.acMatches = DonTopo::luaApiMatches(frag.text);

                    tab.acVisible = !tab.acMatches.empty();
                    if (tab.acVisible)
                    {
                        tab.acSelected = 0;
                        // frag.startColumn is a real character index; acFragmentStart
                        // is used as Coordinates (visual column) in SetSelection/Delete/
                        // SetCursorPosition and in the popup positioning, so it has
                        // to be converted back here, not earlier.
                        int line = tab.editor.GetCursorPosition().mLine;
                        int visualColumn = characterColumnFromIndex(
                            tab.editor, tab.editor.GetCurrentLineText(), frag.startColumn);
                        tab.acFragmentStart = TextEditor::Coordinates(line, visualColumn);
                    }
                }

                if (tab.acVisible)
                {
                    // The widget measures its columns with the width of '#', not that of
                    // 'A' (TextEditor.cpp:856): with a proportional font the
                    // two do not match and the popup drifted away from the caret the
                    // further to the right it was.
                    const float charWidth = ImGui::GetFont()->CalcTextSizeA(
                        ImGui::GetFontSize(), FLT_MAX, -1.0f, "#").x;
                    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
                    // Line-number gutter. mTextStart and mLeftMargin are
                    // private in the widget, so they are recalculated the same way as
                    // there (TextEditor.cpp:889-890), the same workaround, and for the
                    // same reason, as characterIndexFromColumn above.
                    // Without this the popup came out ~35 px to the left ALWAYS.
                    char lineNoBuf[16];
                    snprintf(lineNoBuf, sizeof(lineNoBuf), " %d ", tab.editor.GetTotalLines());
                    const float gutter = ImGui::GetFont()->CalcTextSizeA(
                        ImGui::GetFontSize(), FLT_MAX, -1.0f, lineNoBuf).x + kTextEditorLeftMargin;

                    ImVec2 popupPos(
                        editorOrigin.x + gutter + tab.acFragmentStart.mColumn * charWidth,
                        editorOrigin.y + tab.acFragmentStart.mLine * lineHeight + lineHeight);

                    // The editor's internal scroll cannot be read from outside
                    // (its child is its own and ImGui does not expose it without imgui_internal),
                    // so with the file scrolled the computed position
                    // leaves the panel. Clamping it to the editor's visible rectangle
                    // keeps the popup always in view and stuck to the edge closest
                    // to the caret, instead of drawing it where nobody sees it.
                    const float popupWidth = 420.0f;
                    const float popupMaxHeight = 9.0f * lineHeight;
                    popupPos.x = std::min(std::max(popupPos.x, editorOrigin.x),
                                          std::max(editorEnd.x - popupWidth, editorOrigin.x));
                    popupPos.y = std::min(std::max(popupPos.y, editorOrigin.y),
                                          std::max(editorEnd.y - popupMaxHeight, editorOrigin.y));

                    ImGui::SetNextWindowPos(popupPos);
                    ImGui::SetNextWindowSize(ImVec2(popupWidth, 0.0f));
                    ImGuiWindowFlags acFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing;

                    ImGui::Begin("##ScriptEditorAutocomplete", nullptr, acFlags);
                    int visibleCount = std::min(static_cast<int>(tab.acMatches.size()), 8);
                    ImGui::BeginChild("##ScriptEditorAutocompleteList",
                        ImVec2(0.0f, visibleCount * ImGui::GetTextLineHeightWithSpacing()), false);
                    for (int m = 0; m < static_cast<int>(tab.acMatches.size()); ++m)
                    {
                        const LuaApiMatch& match = tab.acMatches[m];
                        bool selected = (m == tab.acSelected);
                        // The ID goes by index: two suggestions can share visible
                        // text (the same member in two types) and
                        // would collide as a single Selectable.
                        ImGui::PushID(m);
                        if (ImGui::Selectable("##fila", selected))
                        {
                            applyMatch(tab, match);
                            tab.editor.SetHandleKeyboardInputs(true);
                        }
                        // The signature goes on the same line, in gray: the name
                        // alone does not say how many arguments it takes or what it returns.
                        ImGui::SameLine(0.0f, 0.0f);
                        ImGui::TextUnformatted(match.symbol.c_str());
                        if (!match.signature.empty())
                        {
                            ImGui::SameLine(0.0f, 0.0f);
                            ImGui::TextDisabled("%s", match.signature.c_str());
                        }
                        ImGui::PopID();
                        if (selected)
                        {
                            ImGui::SetItemDefaultFocus();
                            // Without this, going down past the eighth row moved
                            // the selection out of the visible part and Enter
                            // inserted something that could not be seen.
                            ImGui::SetScrollHereY(0.5f);
                        }
                    }
                    ImGui::EndChild();
                    // Documentation of the selected suggestion, below the
                    // list: one line, and only the selected one's; putting it
                    // on every row would turn the popup into a wall of text.
                    if (tab.acSelected >= 0 && tab.acSelected < static_cast<int>(tab.acMatches.size()))
                    {
                        const std::string& doc = tab.acMatches[tab.acSelected].doc;
                        if (!doc.empty())
                        {
                            ImGui::Separator();
                            ImGui::PushTextWrapPos(0.0f);
                            ImGui::TextDisabled("%s", doc.c_str());
                            ImGui::PopTextWrapPos();
                        }
                    }
                    ImGui::End();
                }

                drawStatusBar(tab);

                ImGui::EndTabItem();
            }
            ImGui::PopID();

            if (!open)
                closeRequested = i;
        }
        ImGui::EndTabBar();
    }
    m_focusIndex = -1;

    if (closeRequested >= 0)
    {
        if (m_tabs[closeRequested].dirty)
        {
            m_closeConfirmIndex = closeRequested;
            m_openCloseConfirmPopup = true;
        }
        else
            m_tabs.erase(m_tabs.begin() + closeRequested);
    }

    if (m_openCloseConfirmPopup)
    {
        ImGui::OpenPopup("Unsaved changes##ScriptEditor");
        m_openCloseConfirmPopup = false;
    }

    if (ImGui::BeginPopupModal("Unsaved changes##ScriptEditor", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        Tab& tab = m_tabs[m_closeConfirmIndex];
        ImGui::Text("'%s' has unsaved changes.", tab.path.filename().string().c_str());

        if (ImGui::Button("Save"))
        {
            saveTab(tab);
            m_tabs.erase(m_tabs.begin() + m_closeConfirmIndex);
            m_closeConfirmIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard"))
        {
            m_tabs.erase(m_tabs.begin() + m_closeConfirmIndex);
            m_closeConfirmIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_closeConfirmIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace DonTopo
