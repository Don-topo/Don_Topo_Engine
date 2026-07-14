#include "DonTopo/Core/Window.h"
#include <GLFW/glfw3.h>
#include <stb_image.h>
#include <cstdio>
#include <stdexcept>

namespace DonTopo {

Window::~Window() {
    shutdown();
}

void Window::init(int width, int height, const char* title, const char* iconPath) {
    if (!glfwInit())
        throw std::runtime_error("GLFW: failed to initialize");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // Vulkan, sin contexto OpenGL
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);
    // Oculta hasta setear el icono: Windows crea la entrada de la taskbar
    // en cuanto la ventana se hace visible y cachea ese icono inicial —
    // si glfwSetWindowIcon se llama después de que la ventana ya es
    // visible, la barra de título se actualiza (responde a WM_SETICON en
    // cualquier momento) pero la taskbar no siempre refresca.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        throw std::runtime_error("GLFW: failed to create window");
    }

    if (iconPath) {
        int w, h, channels;
        unsigned char* pixels = stbi_load(iconPath, &w, &h, &channels, STBI_rgb_alpha);
        if (pixels) {
            GLFWimage image{ w, h, pixels };
            // count=1: un solo tamaño: GLFW/Windows escala esa imagen para
            // ICON_SMALL (barra de título) e ICON_BIG (barra de tareas).
            glfwSetWindowIcon(m_window, 1, &image);
            stbi_image_free(pixels);
        } else {
            std::fprintf(stderr, "Window: no se pudo cargar el icono '%s'\n", iconPath);
        }
    }

    glfwShowWindow(m_window);
}

void Window::shutdown() {
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
        glfwTerminate();
    }
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(m_window);
}

void Window::pollEvents() const {
    glfwPollEvents();
}

} // namespace DonTopo
