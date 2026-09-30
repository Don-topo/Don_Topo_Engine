#pragma once

namespace DonTopo
{
    // Animator canvas ids (imgui-node-editor). They live here, outside the
    // panel, because they are the risky piece of the canvas: a failure when encoding or
    // decoding gives no compile error, it shows up as "deleting a
    // node deletes another" or "the context menu opens the wrong state's".
    // This way they can be tested without a window.
    //
    // FIVE slots per state: the node, the normal pin pair and a
    // SECONDARY pair used by the return transition when two states link
    // in both directions. Without that second pair the two curves leave from the
    // same two points and are drawn on top of each other, because the curvature
    // (StyleVar_LinkStrength) is a property of the PIN and not of the link.
    //
    // The id that is encoded is the state's **editorId**, not its index: the
    // index changes when removeState reindexes and a survivor would inherit the
    // visual slot (position, selection) of the deleted node, which the library
    // caches by id.
    namespace canvasIds
    {
        inline int node(int eid)       { return eid * 5 + 1; }
        inline int inputPin(int eid)   { return eid * 5 + 2; }
        inline int outputPin(int eid)  { return eid * 5 + 3; }
        inline int inputPin2(int eid)  { return eid * 5 + 4; }
        inline int outputPin2(int eid) { return eid * 5 + 5; }
        inline int link(int transIdx)  { return 100000 + transIdx; }

        // The same integer division works for the five variants.
        inline int editorIdFrom(int rawId) { return (rawId - 1) / 5; }
        // Output: slot 2 (normal pair) and slot 4 (secondary pair).
        inline bool isOutputPin(int pin) { const int r = (pin - 1) % 5; return r == 2 || r == 4; }
        // The two input pins, to tell the secondary pair from the normal one.
        inline bool isSecondaryPin(int pin) { const int r = (pin - 1) % 5; return r == 3 || r == 4; }

        // Any State node: ids OUTSIDE the scheme of the states and the links.
        // They are ALWAYS checked before decoding: passed through editorIdFrom
        // they would match an editorId (180000) that no graph reaches, but
        // isOutputPin would misclassify them.
        inline constexpr int kAnyStateNode   = 900001;
        inline constexpr int kAnyStateOutPin = 900002;
    }
}
