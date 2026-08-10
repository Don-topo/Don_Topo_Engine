#pragma once
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

namespace DonTopo {

// Dispositivo de un binding de acción, ya traducido a códigos GLFW por el
// editor: Core no puede hablar ImGuiKey (split Core/Editor).
enum class ActionDevice { Key, Mouse, Pad };

struct ActionBinding {
    ActionDevice device = ActionDevice::Key;
    int          code   = -1;   // GLFW_KEY_* o GLFW_MOUSE_BUTTON_* según device
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
    // GLFW_GAMEPAD_BUTTON_LAST+1 entradas, del primer mando conectado. Solo lo
    // consumen las acciones: no hay API pública de gamepad todavía.
    static std::array<bool, 15> s_padCurr;
    static std::array<bool, 15> s_padPrev;

    static std::unordered_map<std::string, std::vector<ActionBinding>> s_actions;
    static bool s_actionsLoaded;
    static std::vector<std::string> s_actionDiagnostics;
};

} // namespace DonTopo
