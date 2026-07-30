#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstdint>

struct GLFWwindow;

namespace DonTopo {

    class GameObject;

    // Todo lo que el Renderer necesita de la capa de UI, y nada más. Existe
    // para que el motor no dependa del editor: el Renderer llama a estos
    // hooks sin saber qué biblioteca de UI ni qué paneles hay detrás. En el
    // runtime no hay implementación: el puntero se queda nulo y el pass de UI
    // no se graba.
    class UiLayer {
        public:
            // Lo que el backend de UI necesita del Renderer para arrancar. Va
            // como struct y no como accesores públicos del Renderer para no
            // abrir sus handles de Vulkan a cualquier otro llamante.
            struct InitInfo {
                GLFWwindow*      window         = nullptr;
                VkInstance       instance       = VK_NULL_HANDLE;
                VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
                VkDevice         device         = VK_NULL_HANDLE;
                uint32_t         queueFamily    = 0;
                VkQueue          queue          = VK_NULL_HANDLE;
                uint32_t         imageCount     = 0;
                // Pass del swapchain donde se dibuja la UI (pass 2).
                VkRenderPass     renderPass     = VK_NULL_HANDLE;
            };

            virtual ~UiLayer() = default;

            virtual void initUi(const InitInfo& info)   = 0;
            virtual void shutdownUi()                   = 0;
            // Registra la imagen offscreen de la escena y devuelve el
            // descriptor set con el que la UI la muestreará.
            virtual VkDescriptorSet registerUiTexture(VkSampler sampler, VkImageView view) = 0;
            virtual void unregisterUiTexture(VkDescriptorSet set) = 0;
            // Construye el frame de UI. Se llama ANTES de grabar el command
            // buffer: aquí es donde la UI puede voltear el estado de Play o
            // mutar la escena, y el Renderer lee ambas cosas justo después.
            virtual void buildUiFrame(VkDescriptorSet viewportTexture,
                                      GameObject*     sceneRoot,
                                      const glm::mat4& cameraView) = 0;
            // Graba en cmd la UI ya construida, dentro del pass del swapchain.
            virtual void recordUi(VkCommandBuffer cmd) = 0;
            // Play Mode activo. Lo consulta el Renderer para elegir la cámara
            // del frame (CameraComponent de la escena vs. cámara de vuelo).
            virtual bool isPlaying() const = 0;
    };

}
