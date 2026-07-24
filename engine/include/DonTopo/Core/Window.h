#pragma once

struct GLFWwindow;

namespace DonTopo {

class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    // iconPath: ruta a un PNG (RGBA o no) para el icono de la ventana y de
    // la barra de tareas (glfwSetWindowIcon). nullptr = icono por defecto
    // del sistema.
    //
    // showOnInit=false deja la ventana OCULTA al volver: el caller la enseña
    // con show() cuando tenga algo que pintar. Es el remedio que recomienda
    // GLFW para el "white flash" del arranque — entre que la ventana se hace
    // visible y que se presenta el primer frame, Windows pinta el area de
    // cliente con el fondo por defecto (blanco). El runtime tarda ~520ms en
    // levantar Vulkan, asi que ese blanco se ve de sobra. El editor usa el
    // default (true) y no cambia.
    void init(int width, int height, const char* title, const char* iconPath = nullptr,
              bool showOnInit = true);
    void shutdown();

    // Hace visible la ventana. Idempotente (glfwShowWindow lo es). Solo hace
    // falta si se llamo a init con showOnInit=false.
    void show() const;

    bool shouldClose() const;
    void pollEvents() const;

    GLFWwindow* getNativeWindow() const { return m_window; }

private:
    GLFWwindow* m_window = nullptr;
};

} // namespace DonTopo
