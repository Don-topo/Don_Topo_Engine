#include "DonTopo/Core/Input.h"
#include <GLFW/glfw3.h>

namespace DonTopo
{
    GLFWwindow* Input::s_window = nullptr;
    std::array<bool, 349> Input::s_curr{};
    std::array<bool, 349> Input::s_prev{};

    void Input::init(GLFWwindow* window) { s_window = window; }

    void Input::update()
    {
        if (!s_window) return;
        s_prev = s_curr;
        for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k)
            s_curr[k] = glfwGetKey(s_window, k) == GLFW_PRESS;
    }

    bool Input::isKeyDown(int key)
    {
        return key >= 0 && key <= GLFW_KEY_LAST && s_curr[key];
    }
    bool Input::isKeyPressed(int key)
    {
        return key >= 0 && key <= GLFW_KEY_LAST && s_curr[key] && !s_prev[key];
    }
    bool Input::isKeyReleased(int key)
    {
        return key >= 0 && key <= GLFW_KEY_LAST && !s_curr[key] && s_prev[key];
    }
    bool Input::isMouseButtonDown(int button)
    {
        return s_window && glfwGetMouseButton(s_window, button) == GLFW_PRESS;
    }
}
