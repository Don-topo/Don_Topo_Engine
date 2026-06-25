#include "DonTopo/Window.h"
#include <GLFW/glfw3.h>
#include <stdexcept>

namespace DonTopo {

Window::~Window() {
    shutdown();
}

void Window::init(int width, int height, const char* title) {
    if (!glfwInit())
        throw std::runtime_error("GLFW: failed to initialize");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // Vulkan, sin contexto OpenGL
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);

    m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        throw std::runtime_error("GLFW: failed to create window");
    }
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
