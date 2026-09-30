#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace DonTopo {

// Ring buffer of confirmed edit actions, most recent at the end.
// No persistence to disk (there is nothing to save).
class LogPanel {
public:
    void push(const std::string& message);
    // Same as push(message) but tagging the entry with a module
    // ("Renderer", "Physics", ...). The module is drawn as a colored chip
    // in front of the message; the color comes from a hash of the name, so a new
    // module does not force touching the panel.
    void push(const std::string& message, const std::string& module);
    void draw();
    bool* GetOpenPtr() { return &m_open; }

    // Read access to the buffer. It exists so that the per-line splitting of push()
    // can be checked without a window or an ImGui context: the panel cannot
    // be drawn in a test, but the buffer can be inspected.
    size_t             entryCount() const { return m_entries.size(); }
    const std::string& entryMessage(size_t index) const { return m_entries[index].message; }
    const std::string& entryModule(size_t index) const { return m_entries[index].module; }

    // Module of the entries that arrive without one (all current callers
    // of pushLog): no chip, they are drawn exactly as before.
    static constexpr const char* kDefaultModule = "General";

private:
    struct Entry {
        std::string prefix;   // "[HH:MM:SS] "
        std::string message;
        std::string module;
        // The selection lives in the entry, not in a separate index: this way
        // it survives the ring buffer's pop_front without shifting.
        bool     selected = false;
        uint64_t id       = 0;
    };

    void drawRow(size_t index);
    void handleRowClick(size_t index);
    // Copies the selection to the clipboard; if nothing is selected, copies
    // everything that is being shown.
    void copySelection();

    static constexpr size_t kLogMaxEntries = 200;
    std::deque<Entry> m_entries;
    // Shift+click anchor, stored by id (not by index: the index
    // shifts when the ring buffer discards the oldest entries).
    uint64_t m_anchorId = 0;
    uint64_t m_nextId   = 1;
    // true if the panel was already scrolled to the bottom the previous frame.
    // It avoids fighting the user if they scroll up to read history while more
    // lines arrive.
    bool m_autoScroll = true;
    bool m_open = true;
};

} // namespace DonTopo
