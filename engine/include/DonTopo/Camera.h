#pragma once
#include <glm/glm.hpp>

struct GLFWwindow;

namespace DonTopo
{
    class Camera
    {
        public:
            Camera(glm::vec3 position = {0,0,3}, float yaw = -90.0f, float pitch = 0.0f);
            void update(GLFWwindow* window, float deltaTime);
            void processMouse(float xOffset, float yOffset);
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