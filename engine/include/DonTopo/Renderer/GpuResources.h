#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <cstdint>

namespace DonTopo {

class GpuDevice;
class TransferBatch;

class GpuResources {
public:
    explicit GpuResources(const GpuDevice& gpu) : m_gpu(gpu) {}
    GpuResources(const GpuResources&)            = delete;
    GpuResources& operator=(const GpuResources&) = delete;

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props,
                      VkBuffer& buf, VkDeviceMemory& mem);
    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size,
                    TransferBatch* batch = nullptr);
    void uploadBuffer(const void* data, VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VkBuffer& buf, VkDeviceMemory& mem,
                      TransferBatch* batch = nullptr);

    void createImage(uint32_t w, uint32_t h, VkFormat fmt,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags props,
                     VkImage& img, VkDeviceMemory& mem);
    void transitionImageLayout(VkImage img,
                               VkImageLayout from, VkImageLayout to,
                               TransferBatch* batch = nullptr);
    void copyBufferToImage(VkBuffer buf, VkImage img,
                           uint32_t w, uint32_t h,
                           TransferBatch* batch = nullptr);

    void createTextureImage(const std::string& path,
                            const std::vector<uint8_t>& embedded,
                            VkImage& img, VkDeviceMemory& mem,
                            TransferBatch* batch = nullptr);
    void createNormalMapImage(const std::string& path,
                              const std::vector<uint8_t>& embedded,
                              VkImage& img, VkDeviceMemory& mem,
                              TransferBatch* batch = nullptr);
    void createSolidColorImage(const uint8_t rgba[4],
                               VkImage& img, VkDeviceMemory& mem,
                               TransferBatch* batch = nullptr);

    // Variantes que reciben los píxeles ya decodificados por el worker. Son las
    // que usa la carga asíncrona: repetir el stbi_load en el hilo principal
    // tiraría por tierra la mitad de la ganancia.
    void createTextureImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h,
                                      VkImage& img, VkDeviceMemory& mem,
                                      TransferBatch* batch = nullptr);
    void createNormalMapImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h,
                                        VkImage& img, VkDeviceMemory& mem,
                                        TransferBatch* batch = nullptr);
    void createTextureImageView(VkImage img, VkImageView& view,
                                VkFormat fmt = VK_FORMAT_R8G8B8A8_SRGB);
    // Crea un sampler NUEVO, que pasa a ser del llamante y este debe destruir.
    // Para las texturas de un material NO se usa: ver sharedMaterialSampler.
    void createTextureSampler(VkSampler& out);

    // El sampler de las texturas de material. UNO para todo el motor, prestado:
    // el llamante NO debe destruirlo.
    //
    // createTextureSampler no recibe un solo parametro, o sea que todos los
    // samplers que produce son byte a byte identicos. Aun asi se creaba uno por
    // TEXTURA y por MALLA —difusa, normal y ORM: tres por malla—, y eso no es
    // solo desperdicio: maxSamplerAllocationCount suele valer 4000, asi que una
    // escena de ~1330 mallas se quedaba sin samplers y fallaba al cargar en una
    // GPU donde sobra memoria. Compartirlo quita ese techo entero.
    //
    // Se crea la primera vez que se pide y lo destruye destroySharedSampler.
    VkSampler sharedMaterialSampler();
    // En el teardown del Renderer, con el device todavia vivo.
    void destroySharedSampler();

private:
    const GpuDevice& m_gpu;
    VkSampler        m_materialSampler = VK_NULL_HANDLE;
};

} // namespace DonTopo
