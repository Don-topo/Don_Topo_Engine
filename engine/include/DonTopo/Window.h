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
    void init(int width, int height, const char* title, const char* iconPath = nullptr);
    void shutdown();

    bool shouldClose() const;
    void pollEvents() const;

    GLFWwindow* getNativeWindow() const { return m_window; }

private:
    GLFWwindow* m_window = nullptr;
};

} // namespace DonTopo
