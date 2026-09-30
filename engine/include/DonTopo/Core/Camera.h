#pragma once
#include <glm/glm.hpp>

struct GLFWwindow;

namespace DonTopo
{
    class Camera
    {
        public:
            Camera(glm::vec3 position = {0,0,3}, float yaw = -90.0f, float pitch = 0.0f);
            // keyboardEnabled=false leaves the KEYBOARD out (W/A/S/D/Q/E) but not
            // the gamepad: the editor frees those letters for the gizmo shortcuts
            // while the right button is not held, and the gamepad does not compete
            // with any shortcut, so it keeps moving the camera always.
            // Default true: the runtime and the tests do not change.
            void update(GLFWwindow* window, float deltaTime, bool keyboardEnabled = true);
            void processMouse(float xOffset, float yOffset);
            // Reorients the camera to look along the given axis (used by the
            // viewport axis gizmo); it only rotates, it does not change position.
            void lookAlongAxis(const glm::vec3& axis);
            // Repositions the camera to frame an object: it backs off along
            // the current camera->center vector a distance proportional to
            // boundingRadius, and ends up looking straight at center (used by "F"
            // to center on the selected GameObject).
            void focusOn(const glm::vec3& center, float boundingRadius);
            glm::mat4 getViewMatrix() const;
            float getFov() const { return m_fov; }
            glm::vec3 getPos()   const { return m_pos;   }
            glm::vec3 getFront() const { return m_front; }
            glm::vec3 getUp()    const { return m_up;    }
            float moveSpeed   = 50.0f;
            float mouseSens   = 0.1f;
            float gamepadSens = 150.0f;

        private:
            glm::vec3 m_pos;
            glm::vec3 m_front;
            glm::vec3 m_up;
            float m_yaw;
            float m_pitch;
            float m_fov = 45.0f;

            void updateVectors();
    };
}