#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include "DonTopo/Renderer/TextureImport.h"

namespace DonTopo {

class GpuDevice;
class TransferBatch;

// A rectangular region of RGBA8 pixels that replaces another inside an
// already created image. It is what the thumbnail atlas needs: copy ONE cell
// without re-uploading the atlas's 16 MB.
struct ImageTileUpload
{
    uint32_t       x = 0, y = 0;    // top-left corner inside the image
    uint32_t       w = 0, h = 0;
    const uint8_t* rgba = nullptr;  // w*h*4 bytes
};

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
                     VkImage& img, VkDeviceMemory& mem,
                     uint32_t mipLevels = 1);
    void transitionImageLayout(VkImage img,
                               VkImageLayout from, VkImageLayout to,
                               TransferBatch* batch = nullptr);
    void copyBufferToImage(VkBuffer buf, VkImage img,
                           uint32_t w, uint32_t h,
                           TransferBatch* batch = nullptr);

    // Uploads `w * h * 4` bytes of RGBA to a NEW image, with its staging, its
    // copy and its two transitions. It is the body that was copied in SEVEN
    // places (the five create*Image here, ensurePlaceholder and
    // UiTextureAtlas::loadFromPixels), and the only thing that varied between them was
    // where the pixels come from and the format. The pixels are copied inside,
    // so the caller can free them on return.
    //
    // Without `batch`, the three operations go in ONE command buffer and therefore in
    // ONE wait, not three: measured at 0.41 ms per wait, that is ~1.2 ms per
    // image before and ~0.4 after. With `batch` there is no wait at all: the submit
    // and the fence belong to whoever owns it, and the image is not readable until it
    // signals.
    //
    // `mips` are levels 1..N-1 of the chain (0 is `pixels`); they go in the
    // SAME staging and the SAME copy, and the barrier covers all the levels.
    void uploadPixelsToImage(const void* pixels, uint32_t w, uint32_t h, VkFormat fmt,
                             VkImage& img, VkDeviceMemory& mem,
                             TransferBatch* batch = nullptr,
                             const TextureMip* mips = nullptr, size_t mipCount = 0);

    // NEW image, fully transparent and already in SHADER_READ_ONLY_OPTIMAL. For
    // textures that will be filled in by regions (uploadPixelsToImageRegions).
    void createBlankImage(uint32_t w, uint32_t h, VkFormat fmt,
                          VkImage& img, VkDeviceMemory& mem,
                          TransferBatch* batch = nullptr);

    // Copies N regions to an image that is already in SHADER_READ_ONLY_OPTIMAL and
    // leaves it the same. A single staging, a single command buffer and (without batch) a single
    // wait for the N regions. The entry barrier covers the shader
    // reads of frames already submitted to the same queue.
    void uploadPixelsToImageRegions(VkImage img, const ImageTileUpload* tiles, size_t count,
                                    TransferBatch* batch = nullptr);

    // `outFormat` (optional) receives the format the image ended up with: it is
    // decided by resolveSrgb (slot + sidecar) and the VIEW has to declare the same one.
    void createTextureImage(const std::string& path,
                            const std::vector<uint8_t>& embedded,
                            VkImage& img, VkDeviceMemory& mem,
                            TransferBatch* batch = nullptr,
                            VkFormat* outFormat = nullptr);
    void createNormalMapImage(const std::string& path,
                              const std::vector<uint8_t>& embedded,
                              VkImage& img, VkDeviceMemory& mem,
                              TransferBatch* batch = nullptr,
                              VkFormat* outFormat = nullptr);
    void createSolidColorImage(const uint8_t rgba[4],
                               VkImage& img, VkDeviceMemory& mem,
                               TransferBatch* batch = nullptr);

    // Variants that receive the pixels already decoded by the worker. They are
    // the ones used by async loading: repeating the stbi_load on the main
    // thread would throw away half the gain.
    // The format is decided by the caller (resolveSrgb over the slot and the settings
    // the worker brought); the mips are levels 1..N-1.
    void createMaterialImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h, VkFormat fmt,
                                       const TextureMip* mips, size_t mipCount,
                                       VkImage& img, VkDeviceMemory& mem,
                                       TransferBatch* batch = nullptr);
    void createTextureImageView(VkImage img, VkImageView& view,
                                VkFormat fmt = VK_FORMAT_R8G8B8A8_SRGB);
    // Creates a NEW sampler, which becomes the caller's and which the caller must destroy.
    // It is NOT used for a material's textures: see sharedMaterialSampler.
    void createTextureSampler(VkSampler& out);

    // The sampler for material textures. ONE for the whole engine, lent out:
    // the caller must NOT destroy it.
    //
    // createTextureSampler takes not a single parameter, so all the
    // samplers it produces are byte-for-byte identical. Even so, one was created per
    // TEXTURE and per MESH (diffuse, normal and ORM: three per mesh), and that is not
    // just waste: maxSamplerAllocationCount is usually 4000, so a
    // scene of ~1330 meshes ran out of samplers and failed to load on a
    // GPU with plenty of spare memory. Sharing it removes that ceiling entirely.
    //
    // It is created the first time it is requested and destroySharedSampler destroys it.
    VkSampler sharedMaterialSampler();
    // In the Renderer teardown, with the device still alive.
    void destroySharedSampler();

    // ── Shared filler textures ──────────────────────────────────────────────
    //
    // A mesh WITHOUT a material used to receive its OWN three filler images
    // (1x1 white, flat normal, white ORM), each with its own memory
    // allocation and its staging buffer, which also allocates. Six
    // allocations per mesh to paint the same few pixels over and over.
    // And `maxMemoryAllocationCount` is usually 4096 (see H72).
    //
    // Now they exist ONCE and are lent out. The three create* below
    // return them on their own when the material asks for no texture; the caller does not choose,
    // so the criterion for "this is filler" lives in a single place.
    //
    // Whoever receives them must NOT destroy them: that is what releaseMaterialImage is for,
    // which is the ONLY place that knows how to tell them apart. There are three paths that
    // free material textures (static mesh, character, and the deferred
    // delete when a texture is changed from the editor) and all three go through it.
    // The white UNORM of the ORM slot. It is the only one of the three that is requested by hand:
    // createSolidColorImage is not touched because UiSpriteBatch uses it and DOES destroy
    // its own.
    void sharedWhiteOrm(VkImage& img, VkDeviceMemory& mem);
    bool isSharedPlaceholder(VkImage img) const;
    // Have the three fillers already been released? From then on NOBODY should be
    // freeing material textures: the fillers are the last ones.
    bool placeholdersDestroyed() const { return m_placeholdersDestroyed; }
    // Destroys image and memory EXCEPT if they are lent. The VIEW does not count:
    // that one does belong to each mesh (it is created with createTextureImageView) and is destroyed
    // by the caller as usual.
    void releaseMaterialImage(VkImage img, VkDeviceMemory mem);
    void destroySharedPlaceholders();

private:
    // Uploads one of the three fillers the first time it is needed.
    void ensurePlaceholder(VkImage& img, VkDeviceMemory& mem,
                           const uint8_t rgba[4], VkFormat fmt);

    const GpuDevice& m_gpu;
    VkSampler        m_materialSampler = VK_NULL_HANDLE;

    // White in SRGB (diffuse), flat normal in UNORM, and white in UNORM (ORM).
    // The format matters: the image is not created with MUTABLE_FORMAT, so the
    // view has to use EXACTLY the same one it was created with.
    VkImage        m_whiteSrgb        = VK_NULL_HANDLE;
    VkDeviceMemory m_whiteSrgbMem     = VK_NULL_HANDLE;
    VkImage        m_flatNormal       = VK_NULL_HANDLE;
    VkDeviceMemory m_flatNormalMem    = VK_NULL_HANDLE;
    VkImage        m_whiteUnorm       = VK_NULL_HANDLE;
    VkDeviceMemory m_whiteUnormMem    = VK_NULL_HANDLE;
    // isSharedPlaceholder decides by comparing against the three handles above,
    // and destroySharedPlaceholders sets them to VK_NULL_HANDLE: without this flag, the
    // guard turns itself off and the following material releases destroy them
    // A SECOND TIME, silently (H79).
    bool           m_placeholdersDestroyed = false;
};

} // namespace DonTopo
