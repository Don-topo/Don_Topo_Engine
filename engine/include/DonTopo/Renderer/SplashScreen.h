#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <cstdint>

namespace DonTopo {

class GpuDevice;

// Carga un PNG a RGBA8 (4 canales). false si no existe o no decodifica, sin
// tocar los parametros de salida. Funcion libre (sin Vulkan) para poder
// testear la garantia de "logo ausente no bloquea" sin device.
bool loadSplashImage(const std::string& path, std::vector<uint8_t>& outRGBA,
                     int& outW, int& outH);

class SplashScreen {
public:
    SplashScreen()                               = default;
    SplashScreen(const SplashScreen&)            = delete;
    SplashScreen& operator=(const SplashScreen&) = delete;

    // Sube el logo y crea el pipeline sobre renderPass. false si el logo no
    // carga (el caller se salta el splash). No lanza por logo ausente.
    bool init(GpuDevice& gpu, VkRenderPass renderPass, VkFormat colorFormat,
              const std::string& logoPath);
    void shutdown(GpuDevice& gpu);

    // Graba el draw del splash. alpha [0,1] para el fade; screenAspect =
    // ancho/alto de la ventana (para el letterbox).
    void recordDraw(VkCommandBuffer cmd, float alpha, float screenAspect);

    bool isInitialized() const { return m_pipeline != VK_NULL_HANDLE; }

private:
    void createDescriptors(GpuDevice& gpu);
    void createPipeline(GpuDevice& gpu, VkRenderPass renderPass);
    // Destruye incondicionalmente cualquier handle no-null y los deja en
    // VK_NULL_HANDLE. vkDestroy*/vkFree* con VK_NULL_HANDLE es no-op valido,
    // asi que sirve tanto para el shutdown normal como para el rollback tras
    // una excepcion a mitad de init().
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
