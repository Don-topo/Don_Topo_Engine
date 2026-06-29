#include "DonTopo/Camera.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace DonTopo
{
    Camera::Camera(glm::vec3 position, float yaw, float pitch) 
        : m_pos(position), m_yaw(yaw), m_pitch(pitch)
    {
        m_up = glm::vec3(0.0f, 1.0f, 0.0f);
        updateVectors();
    }

    void Camera::update(GLFWwindow* window, float deltaTime)
    {
        float velocity = moveSpeed * deltaTime;
        glm::vec3 right = glm::normalize(glm::cross(m_front, m_up));

        // Keyboard
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) m_pos += m_front * velocity;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) m_pos -= m_front * velocity;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) m_pos -= right * velocity;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) m_pos += right * velocity;
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) m_pos += m_up * velocity;
        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) m_pos -= m_up * velocity;

        // Gamepad
        GLFWgamepadstate gp{};
        if (glfwGetGamepadState(GLFW_JOYSTICK_1, &gp))
        {
            auto dead = [](float v) { return std::abs(v) > 0.15f ? v : 0.0f; };

            float lx = dead(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
            float ly = dead(gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
            float rx = dead(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_X]);
            float ry = dead(gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]);
            float lt = (gp.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]  + 1.0f) * 0.5f;
            float rt = (gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f;

            // Left stick: move
            m_pos += m_front * (-ly * velocity);
            m_pos += right   * ( lx * velocity);

            // Triggers: up / down
            m_pos += m_up * ((rt - lt) * velocity);

            // Right stick: look
            processMouse(rx * gamepadSens * deltaTime, ry * gamepadSens * deltaTime);
        }
    }

    void Camera::processMouse(float xOffset, float yOffset)
    {
        m_yaw   += xOffset * mouseSens;
        m_pitch -= yOffset * mouseSens;
        m_pitch  = std::clamp(m_pitch, -89.0f, 89.0f);
        updateVectors();
    }

    glm::mat4 Camera::getViewMatrix() const
    {
        return glm::lookAt(m_pos, m_pos + m_front, m_up);
    }

    void Camera::updateVectors()
    {
        glm::vec3 front;
        front.x = cos(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
        front.y = sin(glm::radians(m_pitch));
        front.z = sin(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
        m_front = glm::normalize(front);
    }
}
