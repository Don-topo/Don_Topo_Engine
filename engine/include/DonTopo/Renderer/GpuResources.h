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

    // Sube `w * h * 4` bytes de RGBA a una imagen NUEVA, con su staging, su
    // copia y sus dos transiciones. Es el cuerpo que estaba copiado en SIETE
    // sitios —las cinco create*Image de aquí, ensurePlaceholder y
    // UiTextureAtlas::loadFromPixels—, y lo único que variaba entre ellos era
    // de dónde salen los píxeles y el formato. Los píxeles se copian dentro,
    // así que el llamante puede liberarlos al volver.
    //
    // Sin `batch`, las tres operaciones van en UN command buffer y por tanto en
    // UNA espera, no en tres: medido a 0,41 ms por espera, o sea ~1,2 ms por
    // imagen antes y ~0,4 después. Con `batch` no hay espera ninguna: el submit
    // y la fence son de quien lo posee, y la imagen no es legible hasta que
    // señale.
    void uploadPixelsToImage(const void* pixels, uint32_t w, uint32_t h, VkFormat fmt,
                             VkImage& img, VkDeviceMemory& mem,
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

    // ── Texturas de relleno compartidas ─────────────────────────────────────
    //
    // Una malla SIN material recibia hasta ahora sus PROPIAS tres imagenes de
    // relleno —1x1 blanca, normal plana, ORM blanca—, cada una con su
    // asignacion de memoria y su buffer de staging, que tambien asigna. Seis
    // asignaciones por malla para pintar los mismos pocos pixeles una y otra
    // vez. Y `maxMemoryAllocationCount` suele valer 4096 (ver H72).
    //
    // Ahora existen UNA vez y se prestan. Los tres create* de abajo las
    // devuelven solas cuando el material no pide textura; el llamante no elige,
    // asi que el criterio de "esto es relleno" vive en un solo sitio.
    //
    // Quien las recibe NO debe destruirlas: para eso esta releaseMaterialImage,
    // que es el UNICO sitio que sabe distinguirlas. Hay tres caminos que
    // liberan texturas de material (malla estatica, personaje, y el borrado
    // diferido al cambiar una textura desde el editor) y los tres pasan por el.
    // La blanca UNORM del slot ORM. Es la unica de las tres que se pide a mano:
    // createSolidColorImage no se toca porque UiSpriteBatch la usa y SI destruye
    // la suya.
    void sharedWhiteOrm(VkImage& img, VkDeviceMemory& mem);
    bool isSharedPlaceholder(VkImage img) const;
    // ¿Ya se soltaron las tres de relleno? A partir de ahí NADIE debería estar
    // liberando texturas de material: los rellenos son las últimas.
    bool placeholdersDestroyed() const { return m_placeholdersDestroyed; }
    // Destruye imagen y memoria SALVO que sean prestadas. La VISTA no entra:
    // esa si es de cada malla —se crea con createTextureImageView— y la destruye
    // el llamante como siempre.
    void releaseMaterialImage(VkImage img, VkDeviceMemory mem);
    void destroySharedPlaceholders();

private:
    // Sube una de las tres de relleno la primera vez que hace falta.
    void ensurePlaceholder(VkImage& img, VkDeviceMemory& mem,
                           const uint8_t rgba[4], VkFormat fmt);

    const GpuDevice& m_gpu;
    VkSampler        m_materialSampler = VK_NULL_HANDLE;

    // Blanca en SRGB (difusa), normal plana en UNORM, y blanca en UNORM (ORM).
    // El formato importa: la imagen no se crea con MUTABLE_FORMAT, asi que la
    // vista tiene que usar EXACTAMENTE el mismo con el que se creo.
    VkImage        m_whiteSrgb        = VK_NULL_HANDLE;
    VkDeviceMemory m_whiteSrgbMem     = VK_NULL_HANDLE;
    VkImage        m_flatNormal       = VK_NULL_HANDLE;
    VkDeviceMemory m_flatNormalMem    = VK_NULL_HANDLE;
    VkImage        m_whiteUnorm       = VK_NULL_HANDLE;
    VkDeviceMemory m_whiteUnormMem    = VK_NULL_HANDLE;
    // isSharedPlaceholder decide comparando contra los tres handles de arriba,
    // y destroySharedPlaceholders los pone a VK_NULL_HANDLE: sin esta marca, la
    // guarda se apaga sola y las siguientes liberaciones de material los
    // destruyen POR SEGUNDA VEZ, en silencio (H79).
    bool           m_placeholdersDestroyed = false;
};

} // namespace DonTopo
