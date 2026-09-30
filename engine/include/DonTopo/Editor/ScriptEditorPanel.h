#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <TextEditor.h>
#include "DonTopo/Scripting/LuaApiReference.h"

namespace DonTopo {

// Dockable panel with tabs of .lua files open for manual editing.
// It knows neither ScriptManager nor GameObject; it only reads/writes text on disk
// (FileManager). The reload in the Lua VM is done by ScriptManager::pollChanges
// on its own (mtime), this panel never calls loadScript.
class ScriptEditorPanel {
public:
    // No-op if path is already open in some tab (that tab gets focus).
    // It also reopens the panel if it was closed (m_open = true): opening a
    // file from Properties/Content Browser must make it visible.
    void openFile(const std::filesystem::path& path);
    void draw();
    // Read/write failures are reported here instead of being silenced
    // (spec: they must show in the editor's Log Console, but this panel does not
    // know EditorUI; EditorUI injects pushLog through this callback).
    void setLogCallback(std::function<void(const std::string&)> cb) { m_log = std::move(cb); }
    // Pointer to the window's visibility flag, used by the checkbox
    // of EditorUI's View menu (ImGui::MenuItem toggles the *bool directly).
    bool* GetOpenPtr() { return &m_open; }

private:
    struct Tab {
        std::filesystem::path path;
        TextEditor editor;
        bool dirty = false;

        // State of the autocomplete popup (Task: diagnostics+autocomplete).
        bool acVisible = false;
        // true after Escape, until the fragment under the cursor changes;
        // it prevents the popup from reopening by itself while the user keeps
        // typing the same word they just dismissed.
        bool acDismissed = false;
        // Exact fragment at the moment of Escape; it allows distinguishing
        // "keep typing the same word" (extends this prefix,
        // stays dismissed) from "change word" (stops
        // extending it, the automatic popup is allowed again).
        std::string acDismissedFragment;
        // Suggestions already resolved by luaApiMatches: each one knows which text to
        // write and from which character of the fragment to substitute it (a
        // suggestion found by member name keeps the 'variable:' the
        // user typed).
        std::vector<LuaApiMatch> acMatches;
        int acSelected = 0;
        TextEditor::Coordinates acFragmentStart;
        std::string acLastFragment;
        // Frames since the last key. The live syntax check
        // waits for typing to stop: recompiling the buffer on every
        // keystroke would draw errors in the middle of every half-typed word.
        // -1 = nothing pending.
        int syntaxDelay = -1;
        // Last known error (1-based line and message); line 0 = no error.
        int errorLine = 0;
        std::string errorMessage;

        // Find / replace. The bar opens with Ctrl+F and closes with
        // Escape; the vendored widget has none of this.
        bool findOpen = false;
        bool findFocusRequested = false;
        char findBuffer[128] = {};
        char replaceBuffer[128] = {};
        bool findCaseSensitive = false;
        std::string findStatus;

        // Go to line (Ctrl+G).
        bool gotoOpen = false;
        int  gotoLine = 1;

        // mtime of the file the last time we read or wrote it. It serves
        // to detect that someone changed it externally (ScriptManager's hot reload
        // reloads the VM, but this tab would keep the old text
        // and on saving would trample the foreign change).
        std::filesystem::file_time_type diskTime {};
        bool externalChange = false;
        // Last observed cursor position; it allows detecting caret movement
        // (e.g. mouse click) that does not trigger IsTextChanged(),
        // to close the popup if it is left with stale coordinates.
        TextEditor::Coordinates acLastCursor;
    };

    void saveTab(Tab& tab);
    // Recompiles the buffer and updates the marker + the bar's error state.
    void refreshDiagnostics(Tab& tab);
    // Re-reads the file from disk, discarding whatever was in the editor.
    void reloadFromDisk(Tab& tab);
    // Find/replace bar and go-to-line; they return true if they have
    // consumed the keyboard this frame (so as not to give it to the editor too).
    bool drawFindBar(Tab& tab);
    void drawStatusBar(Tab& tab);
    // Searches for 'needle' from the cursor; wraps around at the end. Selects
    // what was found and returns true.
    bool findNext(Tab& tab, bool backwards);
    // Writes the chosen suggestion replacing only the piece of the fragment
    // that corresponds to it (see LuaApiMatch::replaceOffset).
    void applyMatch(Tab& tab, const LuaApiMatch& match);
    void log(const std::string& msg) { if (m_log) m_log(msg); }

    std::vector<Tab> m_tabs;
    // Tab index to focus on the next draw() (-1 = none); it is consumed
    // (goes back to -1) after every frame.
    int m_focusIndex = -1;
    // Tab index with the "unsaved changes" popup pending (-1 = none).
    int m_closeConfirmIndex = -1;
    bool m_openCloseConfirmPopup = false;
    // Visibility of the panel window; togglable from EditorUI's View menu
    // via GetOpenPtr(). It does not affect m_tabs: closing the panel only
    // hides the window, the tabs and their state stay in memory.
    bool m_open = true;
    // Request to bring the window to the front, set by openFile and consumed
    // by the next draw(). m_open is not enough: if the panel is docked
    // in a tab group, "open" only means the tab exists, and the file
    // opened behind whichever tab had focus.
    bool m_focusWindowRequested = false;
    std::function<void(const std::string&)> m_log;
};

} // namespace DonTopo
