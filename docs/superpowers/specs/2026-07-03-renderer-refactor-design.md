# Renderer Modular Refactor Design

> **For agentic workers:** Use superpowers:writing-plans then superpowers:executing-plans to implement this spec.

**Goal:** Split `Renderer.cpp` (2560 lines) into three focused modules — `GpuDevice`, `GpuResources`, and a leaner `Renderer` coordinator — without changing the public API.

**Architecture:** `Renderer` composes `GpuDevice` (Vulkan core handles) and `GpuResources` (buffer/image/texture utilities). The public `Renderer` API is identical to today; only internals move.

**Tech Stack:** C++20, Vulkan 1.2, GLFW, GLM

---

## Global Constraints

- Public API of `Renderer` must not change — `Engine.cpp`, `sandbox/main.cpp` compile unmodified
- `Renderer.h` structs (`RenderObject`, `SkinnedRenderObject`, `PushData`, `ComputePush`, `SkinnedMatGfx`, `SubMeshDraw`) stay in `Renderer.h`
- No new dependencies
- All existing features (PBR, shadow, compute skinning, multi-material) must work after refactor

---

## Module Definitions

### GpuDevice

**File:** `engine/include/DonTopo/GpuDevice.h`, `engine/src/GpuDevice.cpp`

Owns and initialises the Vulkan core: instance, debug messenger, surface, physical device, logical device, graphics/present queues, command pool.

```cpp
namespace DonTopo {

class GpuDevice {
public:
    GpuDevice() = default;
    ~GpuDevice() { shutdown(); }
    GpuDevice(const GpuDevice&)            = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;

    void init(GLFWwindow* window);
    void shutdown();

    // Accessors
    VkDevice         device()         const { return m_device; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkQueue          graphicsQueue()  const { return m_graphicsQueue; }
    VkQueue          presentQueue()   const { return m_presentQueue; }
    VkCommandPool    commandPool()    const { return m_commandPool; }
    VkSurfaceKHR     surface()        const { return m_surface; }
    uint32_t         graphicsFamily() const { return m_graphicsFamily; }
    uint32_t         presentFamily()  const { return m_presentFamily; }

    // Utilities used by GpuResources and Renderer
    uint32_t        findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props) const;
    VkCommandBuffer beginOneTimeCommands() const;
    void            endOneTimeCommands(VkCommandBuffer cmd) const;

private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface(GLFWwindow* window);
    void pickPhysicalDevice();
    void createDevice();
    void createCommandPool();

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT*,
        void*);

    VkInstance               m_instance       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR             m_surface        = VK_NULL_HANDLE;
    VkPhysicalDevice         m_physicalDevice = VK_NULL_HANDLE;
    VkDevice                 m_device         = VK_NULL_HANDLE;
    VkQueue                  m_graphicsQueue  = VK_NULL_HANDLE;
    VkQueue                  m_presentQueue   = VK_NULL_HANDLE;
    VkCommandPool            m_commandPool    = VK_NULL_HANDLE;
    uint32_t                 m_graphicsFamily = 0;
    uint32_t                 m_presentFamily  = 0;
};

} // namespace DonTopo
```

**Source of truth for handles:** `Renderer` will store a `GpuDevice m_gpu` member. All code that previously accessed `m_device`, `m_physicalDevice`, etc. will now call `m_gpu.device()`, `m_gpu.physicalDevice()`, etc.

---

### GpuResources

**File:** `engine/include/DonTopo/GpuResources.h`, `engine/src/GpuResources.cpp`

Stateless utility class for buffer and image allocation. Holds a `const GpuDevice&` reference; never owns Vulkan handles.

```cpp
namespace DonTopo {

class GpuResources {
public:
    explicit GpuResources(const GpuDevice& gpu) : m_gpu(gpu) {}
    GpuResources(const GpuResources&)            = delete;
    GpuResources& operator=(const GpuResources&) = delete;

    // Buffers
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props,
                      VkBuffer& buf, VkDeviceMemory& mem);
    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);
    void uploadBuffer(const void* data, VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VkBuffer& buf, VkDeviceMemory& mem);

    // Images
    void createImage(uint32_t w, uint32_t h, VkFormat fmt,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags props,
                     VkImage& img, VkDeviceMemory& mem);
    void transitionImageLayout(VkImage img,
                               VkImageLayout from, VkImageLayout to);
    void copyBufferToImage(VkBuffer buf, VkImage img,
                           uint32_t w, uint32_t h);

    // Textures (load from path or embedded bytes)
    void createTextureImage(const std::string& path,
                            const std::vector<uint8_t>& embedded,
                            VkImage& img, VkDeviceMemory& mem);
    void createNormalMapImage(const std::string& path,
                              const std::vector<uint8_t>& embedded,
                              VkImage& img, VkDeviceMemory& mem);
    void createSolidColorImage(const uint8_t rgba[4],
                               VkImage& img, VkDeviceMemory& mem);
    void createTextureImageView(VkImage img, VkImageView& view,
                                VkFormat fmt = VK_FORMAT_R8G8B8A8_SRGB);
    void createTextureSampler(VkSampler& out);

private:
    const GpuDevice& m_gpu;
};

} // namespace DonTopo
```

---

### Renderer (coordinator)

**File:** `engine/include/DonTopo/Renderer.h` (updated), `engine/src/Renderer.cpp` (reduced)

Composes `GpuDevice` and `GpuResources`. Keeps ownership of: swapchain, render passes, framebuffers, depth resources, sync objects, pipelines, descriptor management, uniform buffers, shadow system, compute skinning, `m_objects`, `m_skinnedObjects`.

Key field changes in `Renderer`:
```cpp
// Replace scattered m_device / m_physicalDevice / m_commandPool / m_graphicsQueue fields with:
GpuDevice    m_gpu;
GpuResources m_res{ m_gpu };  // must be declared after m_gpu
```

All internal calls `createBuffer(...)` → `m_res.createBuffer(...)`, `beginOneTimeCommands()` → `m_gpu.beginOneTimeCommands()`, etc.

Public API (`init`, `drawFrame`, `shutdown`, `setCamera`, `setLights`, `setTransform`, `addSkinnedMesh`, `updateAnimation`, `setSkinnedTransform`, `notifyResize`) does not change.

---

## File Changes Summary

| Action | File |
|--------|------|
| Create | `engine/include/DonTopo/GpuDevice.h` |
| Create | `engine/src/GpuDevice.cpp` |
| Create | `engine/include/DonTopo/GpuResources.h` |
| Create | `engine/src/GpuResources.cpp` |
| Modify | `engine/include/DonTopo/Renderer.h` — add `#include GpuDevice/GpuResources`, replace raw handle fields with `m_gpu`/`m_res` |
| Modify | `engine/src/Renderer.cpp` — remove moved implementations, update all call sites |
| Modify | `engine/CMakeLists.txt` — add `src/GpuDevice.cpp`, `src/GpuResources.cpp` |

---

## Migration Strategy

Move implementations in order — compile-check after each step:

1. Extract `GpuDevice`: move `createInstance`, `setupDebugMessenger`, `createSurface`, `pickPhysicalDevice`, `createDevice`, `createCommandPool`, `findMemoryType`, `beginOneTimeCommands`, `endOneTimeCommands` from Renderer → GpuDevice
2. Add `m_gpu` to Renderer, update all `m_device`/`m_physicalDevice`/etc. references to `m_gpu.device()` etc.
3. Extract `GpuResources`: move the 11 buffer/image/texture method implementations
4. Replace Renderer's direct calls with `m_res.createBuffer(...)` etc.
5. Remove orphaned private declarations from `Renderer.h`
