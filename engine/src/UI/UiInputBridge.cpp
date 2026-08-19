#include "DonTopo/UI/UiInputBridge.h"

#include "DonTopo/Core/Input.h"

#include <GLFW/glfw3.h>

namespace DonTopo
{
    namespace
    {
        // Una tecla y su equivalente en el canvas. isKeyPressed es el FLANCO:
        // repetir una tecla mantenida es cosa de quien la lea, no del canvas
        // (mantener la flecha no debe recorrer el menú a 60 saltos por segundo).
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

        void empuja(UiInputState& out, UiKey key)
        {
            // La misma tecla por dos vías (la cruceta y el stick, o los dos
            // Enter) es UN solo evento: si no, un menú daría dos saltos por
            // pulsación en cuanto alguien use las dos manos.
            for (UiKey k : out.keys)
                if (k == key) return;
            out.keys.push_back(key);
        }
    }

    void fillUiInputKeys(UiInputState& out)
    {
        for (const Atajo& a : kTeclas)
            if (Input::isKeyPressed(a.glfwKey)) empuja(out, a.uiKey);

        for (const Atajo& a : kBotonesPad)
            if (Input::isPadButtonPressed(a.glfwKey)) empuja(out, a.uiKey);

        // Stick izquierdo: los ejes ya vienen digitalizados con histéresis, así
        // que se leen igual que un botón. El eje Y de GLFW crece hacia ABAJO,
        // que es la misma convención que la Y del canvas.
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
