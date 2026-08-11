#pragma once
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
    //
    // ESTE HEADER NO INCLUYE NINGUNA API GRÁFICA a propósito. Lo comparten dos
    // backends —Vulkan y DirectX 12— y meter vulkan.h aquí obligaría a que el
    // editor conociera Vulkan para dibujarse con DX12. Los handles viajan como
    // enteros opacos: quien los pone sabe qué son y quien los consume los
    // devuelve al backend que los creó, sin interpretarlos por el camino.
    class UiLayer {
        public:
            // Con qué API gráfica se ha arrancado. La capa de UI lo necesita
            // para elegir su propio backend (ImGui tiene uno por API).
            enum class GraphicsApi {
                Vulkan,
                D3D12,
            };

            // Lo que el backend de UI necesita del Renderer para arrancar. Va
            // como struct y no como accesores públicos del Renderer para no
            // abrir sus handles a cualquier otro llamante.
            //
            // Los campos son de UNA de las dos APIs según `api`; los de la otra
            // se quedan a cero. Un struct común y no una jerarquía porque el
            // Renderer lo rellena en un sitio y la UI lo lee en otro: partirlo
            // en dos obligaría a downcasts en ambos extremos para no ganar
            // nada.
            struct InitInfo {
                GraphicsApi api    = GraphicsApi::Vulkan;
                GLFWwindow* window = nullptr;

                // --- Vulkan -------------------------------------------------
                // VkInstance, VkPhysicalDevice, VkDevice, VkQueue y VkRenderPass
                // como enteros: en x64 todos caben en 64 bits.
                uint64_t instance       = 0;
                uint64_t physicalDevice = 0;
                uint64_t device         = 0;
                uint32_t queueFamily    = 0;
                uint64_t queue          = 0;
                uint32_t imageCount     = 0;
                // Pass del swapchain donde se dibuja la UI (pass 2).
                uint64_t renderPass     = 0;

                // --- DirectX 12 ---------------------------------------------
                // ID3D12Device*, ID3D12CommandQueue* e ID3D12DescriptorHeap*,
                // más el rango de descriptores que la UI puede repartirse y el
                // formato del render target donde se graba.
                void*    d3dDevice      = nullptr;
                void*    d3dQueue       = nullptr;
                void*    d3dSrvHeap     = nullptr;
                uint64_t d3dSrvCpuStart = 0;
                uint64_t d3dSrvGpuStart = 0;
                uint32_t d3dSrvCount    = 0;
                uint32_t d3dSrvStride   = 0;
                uint32_t d3dRtvFormat   = 0;  // DXGI_FORMAT
                uint32_t framesInFlight = 0;
            };

            virtual ~UiLayer() = default;

            virtual void initUi(const InitInfo& info) = 0;
            virtual void shutdownUi()                 = 0;

            // Registra la imagen offscreen de la escena y devuelve el handle
            // con el que la UI la muestreará.
            //
            // En Vulkan `a` es un VkSampler y `b` un VkImageView, y el retorno
            // es el VkDescriptorSet. En DirectX 12 `a` es el ID3D12Resource* de
            // la textura, `b` no se usa, y el retorno es el descriptor GPU.
            // En los dos casos el valor devuelto acaba en ImGui::Image, que lo
            // trata como opaco.
            virtual uint64_t registerUiTexture(uint64_t a, uint64_t b) = 0;
            virtual void     unregisterUiTexture(uint64_t handle)      = 0;

            // Construye el frame de UI. Se llama ANTES de grabar la lista de
            // comandos: aquí es donde la UI puede voltear el estado de Play o
            // mutar la escena, y el Renderer lee ambas cosas justo después.
            virtual void buildUiFrame(uint64_t         viewportTexture,
                                      GameObject*      sceneRoot,
                                      const glm::mat4& cameraView) = 0;

            // Graba la UI ya construida. `commandList` es un VkCommandBuffer o
            // un ID3D12GraphicsCommandList* según la API con la que se inició.
            virtual void recordUi(void* commandList) = 0;

            // Play Mode activo. Lo consulta el Renderer para elegir la cámara
            // del frame (CameraComponent de la escena vs. cámara de vuelo).
            virtual bool isPlaying() const = 0;
    };

}
