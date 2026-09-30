#pragma once
#include <glm/glm.hpp>
#include <cstdint>

struct GLFWwindow;

namespace DonTopo {

    class GameObject;

    // Everything the Renderer needs from the UI layer, and nothing more. It exists
    // so that the engine does not depend on the editor: the Renderer calls these
    // hooks without knowing which UI library or which panels are behind them. In the
    // runtime there is no implementation: the pointer stays null and the UI pass
    // is not recorded.
    //
    // THIS HEADER INCLUDES NO GRAPHICS API ON PURPOSE. It is shared by two
    // backends (Vulkan and DirectX 12) and putting vulkan.h here would force the
    // editor to know Vulkan in order to draw itself with DX12. Handles travel as
    // opaque integers: whoever sets them knows what they are and whoever consumes them
    // returns them to the backend that created them, without interpreting them along the way.
    class UiLayer {
        public:
            // Which graphics API it was started with. The UI layer needs it
            // to choose its own backend (ImGui has one per API).
            enum class GraphicsApi {
                Vulkan,
                D3D12,
            };

            // What the UI backend needs from the Renderer to start. It goes
            // as a struct and not as public Renderer accessors so as not to
            // open its handles to any other caller.
            //
            // The fields belong to ONE of the two APIs depending on `api`; those of the other
            // stay at zero. A common struct and not a hierarchy because the
            // Renderer fills it in one place and the UI reads it in another: splitting it
            // in two would force downcasts at both ends for no
            // gain.
            struct InitInfo {
                GraphicsApi api    = GraphicsApi::Vulkan;
                GLFWwindow* window = nullptr;

                // --- Vulkan -------------------------------------------------
                // VkInstance, VkPhysicalDevice, VkDevice, VkQueue and VkRenderPass
                // as integers: on x64 they all fit in 64 bits.
                uint64_t instance       = 0;
                uint64_t physicalDevice = 0;
                uint64_t device         = 0;
                uint32_t queueFamily    = 0;
                uint64_t queue          = 0;
                uint32_t imageCount     = 0;
                // Swapchain pass where the UI is drawn (pass 2).
                uint64_t renderPass     = 0;

                // --- DirectX 12 ---------------------------------------------
                // ID3D12Device*, ID3D12CommandQueue* and ID3D12DescriptorHeap*,
                // plus the descriptor range the UI can divide up and the
                // format of the render target it is recorded into.
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

            // Registers the scene's offscreen image and returns the handle
            // with which the UI will sample it.
            //
            // In Vulkan `a` is a VkSampler and `b` a VkImageView, and the return
            // is the VkDescriptorSet. In DirectX 12 `a` is the ID3D12Resource* of
            // the texture, `b` is not used, and the return is the GPU descriptor.
            // In both cases the returned value ends up in ImGui::Image, which
            // treats it as opaque.
            virtual uint64_t registerUiTexture(uint64_t a, uint64_t b) = 0;
            virtual void     unregisterUiTexture(uint64_t handle)      = 0;

            // Builds the UI frame. Called BEFORE recording the command list:
            // this is where the UI can flip the Play state or mutate the scene, and
            // the Renderer reads both right afterwards.
            virtual void buildUiFrame(uint64_t         viewportTexture,
                                      GameObject*      sceneRoot,
                                      const glm::mat4& cameraView) = 0;

            // Records the already built UI. `commandList` is a VkCommandBuffer or
            // an ID3D12GraphicsCommandList* depending on the API it was started with.
            virtual void recordUi(void* commandList) = 0;

            // Play Mode is active. The Renderer queries it to choose the frame's camera
            // (the scene's CameraComponent vs. the fly camera).
            virtual bool isPlaying() const = 0;
    };

}
