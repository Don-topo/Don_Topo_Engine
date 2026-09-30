#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <cstdint>

namespace DonTopo {

class GpuDevice;

// Loads a PNG into RGBA8 (4 channels). false if it does not exist or does not
// decode, without touching the output parameters. Free function (no Vulkan) so
// that the "missing logo does not block" guarantee can be tested without a device.
bool loadSplashImage(const std::string& path, std::vector<uint8_t>& outRGBA,
                     int& outW, int& outH);

class SplashScreen {
public:
    SplashScreen()                               = default;
    SplashScreen(const SplashScreen&)            = delete;
    SplashScreen& operator=(const SplashScreen&) = delete;

    // Uploads the logo and creates the pipeline on renderPass. false if the logo
    // does not load (the caller skips the splash). Does not throw for a missing logo.
    bool init(GpuDevice& gpu, VkRenderPass renderPass, VkFormat colorFormat,
              const std::string& logoPath);
    void shutdown(GpuDevice& gpu);

    // Records the splash draw. alpha [0,1] for the fade; screenAspect =
    // window width/height (for the letterbox).
    void recordDraw(VkCommandBuffer cmd, float alpha, float screenAspect);

    bool isInitialized() const { return m_pipeline != VK_NULL_HANDLE; }

private:
    void createDescriptors(GpuDevice& gpu);
    void createPipeline(GpuDevice& gpu, VkRenderPass renderPass);
    // Unconditionally destroys any non-null handle and leaves them as
    // VK_NULL_HANDLE. vkDestroy*/vkFree* with VK_NULL_HANDLE is a valid no-op,
    // so it serves both for the normal shutdown and for the rollback after
    // an exception halfway through init().
    void destroyResources(GpuDevice& gpu);

    int                   m_logoW      = 0;
    int                   m_logoH      = 0;
    VkImage               m_image      = VK_NULL_HANDLE;
    VkDeviceMemory        m_memory     = VK_NULL_HANDLE;
    VkImageView           m_view       = VK_NULL_HANDLE;
    VkSampler             m_sampler    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet    = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline   = VK_NULL_HANDLE;
};

} // namespace DonTopo
