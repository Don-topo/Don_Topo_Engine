#pragma once
#include <array>

struct GLFWwindow;

namespace DonTopo {

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

private:
    static GLFWwindow* s_window;
    // GLFW_KEY_LAST+1 entradas; índice = keycode GLFW.
    static std::array<bool, 349> s_curr;
    static std::array<bool, 349> s_prev;
};

} // namespace DonTopo
