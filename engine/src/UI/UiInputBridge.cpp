#include "DonTopo/UI/UiInputBridge.h"

#include "DonTopo/Core/Input.h"

#include <GLFW/glfw3.h>

namespace DonTopo
{
    namespace
    {
        // A key and its canvas equivalent. isKeyPressed is the EDGE: repeating
        // a held key is up to whoever reads it, not the canvas
        // (holding the arrow must not step through the menu at 60 jumps per second).
        struct Atajo
        {
            int   glfwKey;
            UiKey uiKey;
        };

        constexpr Atajo kTeclas[] = {
            { GLFW_KEY_TAB,        UiKey::Tab    },
            { GLFW_KEY_ENTER,      UiKey::Enter  },
            { GLFW_KEY_KP_ENTER,   UiKey::Enter  },
            { GLFW_KEY_ESCAPE,     UiKey::Escape },
            { GLFW_KEY_BACKSPACE,  UiKey::Backspace },
            { GLFW_KEY_DELETE,     UiKey::Delete },
            { GLFW_KEY_HOME,       UiKey::Home   },
            { GLFW_KEY_END,        UiKey::End    },
            { GLFW_KEY_LEFT,       UiKey::Left   },
            { GLFW_KEY_RIGHT,      UiKey::Right  },
            { GLFW_KEY_UP,         UiKey::Up     },
            { GLFW_KEY_DOWN,       UiKey::Down   },
        };

        constexpr Atajo kBotonesPad[] = {
            { GLFW_GAMEPAD_BUTTON_DPAD_LEFT,  UiKey::Left   },
            { GLFW_GAMEPAD_BUTTON_DPAD_RIGHT, UiKey::Right  },
            { GLFW_GAMEPAD_BUTTON_DPAD_UP,    UiKey::Up     },
            { GLFW_GAMEPAD_BUTTON_DPAD_DOWN,  UiKey::Down   },
            { GLFW_GAMEPAD_BUTTON_A,          UiKey::Enter  },
            { GLFW_GAMEPAD_BUTTON_B,          UiKey::Escape },
        };

        // Characters accumulated since the last fillUiInputKeys. Single thread:
        // the GLFW callbacks run inside pollEvents, on the main thread.
        std::vector<uint32_t> g_chars;

        // Hard cap. Nobody types 256 characters in one frame; if the buffer gets
        // there, the caller is not draining it, and growing without limit
        // would turn a wiring fault into a memory leak.
        constexpr size_t kMaxChars = 256;

        void empuja(UiInputState& out, UiKey key)
        {
            // The same key through two paths (the d-pad and the stick, or both
            // Enter keys) is ONE event: otherwise a menu would jump twice per
            // press as soon as someone uses both hands.
            for (UiKey k : out.keys)
                if (k == key) return;
            out.keys.push_back(key);
        }
    }

    void pushUiInputChar(uint32_t codepoint)
    {
        if (g_chars.size() >= kMaxChars) return;
        g_chars.push_back(codepoint);
    }

    void discardUiInputChars()
    {
        g_chars.clear();
    }

    void fillUiInputKeys(UiInputState& out)
    {
        // The characters are MOVED and the accumulator is left empty: if they were read
        // without draining, what was typed would repeat on every following frame.
        out.chars.insert(out.chars.end(), g_chars.begin(), g_chars.end());
        g_chars.clear();

        for (const Atajo& a : kTeclas)
            if (Input::isKeyPressed(a.glfwKey)) empuja(out, a.uiKey);

        for (const Atajo& a : kBotonesPad)
            if (Input::isPadButtonPressed(a.glfwKey)) empuja(out, a.uiKey);

        // Left stick: the axes already arrive digitized with hysteresis, so
        // they are read like a button. GLFW's Y axis grows DOWNWARD,
        // which is the same convention as the canvas Y.
        if (Input::isPadAxisPressed(Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_X, true)))
            empuja(out, UiKey::Left);
        if (Input::isPadAxisPressed(Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_X, false)))
            empuja(out, UiKey::Right);
        if (Input::isPadAxisPressed(Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_Y, true)))
            empuja(out, UiKey::Up);
        if (Input::isPadAxisPressed(Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_Y, false)))
            empuja(out, UiKey::Down);

        out.shift = Input::isKeyDown(GLFW_KEY_LEFT_SHIFT)   || Input::isKeyDown(GLFW_KEY_RIGHT_SHIFT);
        out.ctrl  = Input::isKeyDown(GLFW_KEY_LEFT_CONTROL) || Input::isKeyDown(GLFW_KEY_RIGHT_CONTROL);
        out.alt   = Input::isKeyDown(GLFW_KEY_LEFT_ALT)     || Input::isKeyDown(GLFW_KEY_RIGHT_ALT);
    }
}
