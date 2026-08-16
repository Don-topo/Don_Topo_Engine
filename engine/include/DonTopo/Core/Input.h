#pragma once
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

namespace DonTopo {

// Dispositivo de un binding de acción, ya traducido a códigos GLFW por el
// editor: Core no puede hablar ImGuiKey (split Core/Editor).
//
// PadAxis son los sticks y los gatillos: en GLFW son ejes analógicos, no
// botones, así que llevan su propio dispositivo y su propio espacio de códigos
// (ver Input::padAxisCode).
enum class ActionDevice { Key, Mouse, Pad, PadAxis };

struct ActionBinding {
    ActionDevice device = ActionDevice::Key;
    int          code   = -1;   // GLFW_KEY_*, GLFW_MOUSE_BUTTON_*, GLFW_GAMEPAD_BUTTON_*
                                // o código de eje (PadAxis), según device
};

// Fachada estática sobre el teclado/ratón de GLFW con estado prev/curr por
// frame — permite IsKeyPressed/IsKeyReleased (flancos), que glfwGetKey solo
// no da. Solo la usan los bindings de scripting por ahora (Camera sigue con
// glfwGetKey directo — fuera de alcance migrarla).
class Input {
public:
    static void init(GLFWwindow* window);
    // Llamar una vez por frame, antes de ejecutar scripts.
    static void update();

    static bool isKeyDown(int key);      // mantenida
    static bool isKeyPressed(int key);   // solo el frame del flanco de bajada
    static bool isKeyReleased(int key);  // solo el frame del flanco de subida
    static bool isMouseButtonDown(int button);

    // --- Mando (primer mando conectado con mapeo conocido) ---
    // Índice = GLFW_GAMEPAD_BUTTON_*. Sin mando conectado, siempre false. El
    // panel Input Actions los usa para capturar bindings: ImGui no ve el mando
    // salvo que se active NavEnableGamepad, y eso pondría al mando a navegar la
    // interfaz del editor.
    static bool isPadButtonDown(int button);
    static bool isPadButtonPressed(int button);   // solo el frame del flanco

    // --- Ejes del mando (sticks y gatillos) como si fueran botones ---
    // Una dirección de eje es un binding: "stick izquierdo hacia arriba" vale
    // lo mismo que una tecla. El código empaqueta el eje y el signo, porque un
    // binding solo lleva un int y las dos direcciones de un mismo eje son dos
    // acciones distintas.
    static constexpr int kPadAxisBindingCount = 12;   // (GLFW_GAMEPAD_AXIS_LAST+1) * 2
    static constexpr int padAxisCode(int axis, bool negative)
    { return axis * 2 + (negative ? 1 : 0); }
    static constexpr int  padAxisIndex(int code)    { return code / 2; }
    static constexpr bool padAxisNegative(int code) { return (code % 2) != 0; }

    // Digitaliza un eje: valor crudo de GLFW + estado del frame anterior =>
    // estado nuevo. Umbral con histéresis (0.5 activa, 0.4 suelta) para que un
    // stick parado en el borde no dé un flanco por frame. Pública para poder
    // probarla sin mando conectado. Los gatillos vienen en [-1, 1] con el
    // reposo en -1 y se renormalizan aquí a [0, 1].
    static bool padAxisActive(int code, float rawValue, bool wasActive);

    static bool isPadAxisDown(int code);
    static bool isPadAxisPressed(int code);   // solo el frame del flanco

    // --- Acciones con nombre (panel Input Actions del editor) ---
    // Una acción está activa si CUALQUIERA de sus bindings lo está (OR). El
    // mapa se carga de input_actions.json la primera vez que se pregunta;
    // fichero ausente o corrupto => mapa vacío, sin excepción y sin crash.
    // Un nombre desconocido devuelve false: el aviso al Log lo da quien
    // pregunta (Core no tiene canal de log), vía hasAction().
    static bool isActionDown(const std::string& name);
    static bool isActionPressed(const std::string& name);
    static bool isActionReleased(const std::string& name);
    static bool hasAction(const std::string& name);
    // Relee el fichero: el editor la llama al guardar el panel para que una
    // acción recién creada valga en el siguiente Play sin reiniciar.
    static void reloadActions();
    // Avisos acumulados al cargar (bindings de mando ignorados). Se vacía al
    // leerla: el consumidor los vuelca al Log una sola vez.
    static std::vector<std::string> takeActionDiagnostics();

private:
    static void ensureActionsLoaded();
    static void loadActionsFromDisk();

    static GLFWwindow* s_window;
    // GLFW_KEY_LAST+1 entradas; índice = keycode GLFW.
    static std::array<bool, 349> s_curr;
    static std::array<bool, 349> s_prev;
    // GLFW_MOUSE_BUTTON_LAST+1 entradas; hace falta el prev/curr propio porque
    // isMouseButtonDown consulta GLFW directo y no da flancos.
    static std::array<bool, 8> s_mCurr;
    static std::array<bool, 8> s_mPrev;
    // GLFW_GAMEPAD_BUTTON_LAST+1 entradas, del primer mando conectado.
    static std::array<bool, 15> s_padCurr;
    static std::array<bool, 15> s_padPrev;
    // Ejes ya digitalizados; índice = código de padAxisCode().
    static std::array<bool, kPadAxisBindingCount> s_axisCurr;
    static std::array<bool, kPadAxisBindingCount> s_axisPrev;

    static std::unordered_map<std::string, std::vector<ActionBinding>> s_actions;
    static bool s_actionsLoaded;
    static std::vector<std::string> s_actionDiagnostics;
};

} // namespace DonTopo
