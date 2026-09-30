#pragma once
#include "DonTopo/Renderer/MeshKey.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace DonTopo
{
    struct Mesh;

    // The GPU resources that N identical objects share. Everything here
    // is derived ONLY from the content of the Mesh and its Material: nothing per instance
    // (the transform and the name stay in the RenderObject). That is the criterion
    // for deciding whether a field goes in or not: if two equal cubes in different
    // places can share it, it goes in.
    struct SharedGpuMesh
    {
        VkBuffer        vertexBuffer  = VK_NULL_HANDLE;
        VkDeviceMemory  vertexMemory  = VK_NULL_HANDLE;
        VkBuffer        indexBuffer   = VK_NULL_HANDLE;
        VkDeviceMemory  indexMemory   = VK_NULL_HANDLE;
        uint32_t        indexCount    = 0;

        VkImage         textureImage  = VK_NULL_HANDLE;
        VkDeviceMemory  textureMem    = VK_NULL_HANDLE;
        VkImageView     textureView   = VK_NULL_HANDLE;
        VkSampler       sampler       = VK_NULL_HANDLE;

        VkImage         normalImage   = VK_NULL_HANDLE;
        VkDeviceMemory  normalMem     = VK_NULL_HANDLE;
        VkImageView     normalView    = VK_NULL_HANDLE;
        VkSampler       normalSampler = VK_NULL_HANDLE;

        VkImage         ormImage      = VK_NULL_HANDLE;
        VkDeviceMemory  ormMem        = VK_NULL_HANDLE;
        VkImageView     ormView       = VK_NULL_HANDLE;
        VkSampler       ormSampler    = VK_NULL_HANDLE;

        // metallic and roughness used to live here. They moved to the RenderObject
        // (RenderObjects.h) because they are PER OBJECT: while they were in this
        // entry they had to go into the dedup key (two objects with a
        // different finish could not share it), and moving a slider forced
        // re-keying the object and rebuilding its GPU resources, with waitForGpu and
        // re-upload of three textures. That is why the sliders only applied
        // on release.
        //
        // What does belong to the entry is this: whether the material brings an ORM MAP. The
        // texture is shared and its path stays in the key, so the
        // answer holds for all the objects that share the entry. It is what
        // keeps the per-object setter from overriding a map with a
        // slider: with a map, both factors go to 1.0 and the texture rules.
        bool            hasOrmMap     = false;

        // A single descriptor set per entry: its five bindings (UBO, diffuse,
        // normal, shadow, ORM) are identical between objects that share
        // mesh and material. What is per-object goes through push constants.
        VkDescriptorSet descriptorSets[2] = {};
        // Pool they came from. The Renderer chains pools as needed,
        // so freeing them requires remembering which one was theirs.
        VkDescriptorPool descPool         = VK_NULL_HANDLE;

        // AABB in local space, for frustum culling. hasBounds=false (mesh
        // without vertices) means "cannot be bounded": it is always drawn.
        glm::vec3       aabbMin{0.0f};
        glm::vec3       aabbMax{0.0f};
        bool            hasBounds     = false;

        // 0 = uploaded and visible. >0 = waiting for the fence of the batch with that
        // ticket to signal. It lives here and not in the RenderObject because it is the
        // resources that are in flight: a second object that acquires this
        // same entry before the flush has to wait just the same.
        uint64_t        uploadTicket  = 0;
    };

    // Table of shared GPU resources with refcount. It does not know Vulkan beyond
    // the handles: creating and destroying are callbacks of the caller (the Renderer fills them
    // in with its createVertexBuffer/DeferredDelete). That is what makes it
    // testable without a device.
    class SharedGpuMeshCache
    {
        public:
            using Creator   = std::function<void(SharedGpuMesh&)>;
            using Destroyer = std::function<void(const SharedGpuMesh&)>;

            // Returns the index of the entry of `key`, creating it with `create`
            // only the first time. On subsequent calls it increments the
            // refcount and does NOT invoke `create`. createdOut (if passed) says which
            // of the two cases it was: the caller needs it to know whether it has to
            // allocate the descriptor set or whether it already came allocated.
            int acquire(const std::string& key, const Creator& create,
                        bool* createdOut = nullptr);

            // Decrements the refcount. On reaching 0 it removes the entry from the table,
            // frees its slot and passes a COPY of the handles to `destroy`, since the
            // slot can be reused in the same frame while the real destruction
            // is still deferred. No-op if the index is not alive.
            void release(int index, const Destroyer& destroy);

            // Changes the key under which the entry `index` is found, without
            // touching refs or handles. false if the index is not alive or if the
            // new key already belongs to ANOTHER entry: two entries with the same
            // key would leave one unreachable in the map, that is, a leak of
            // GPU resources that nobody would ever free.
            //
            // It is needed by the hot texture swap: when an entry
            // with a single owner changes material, its content stops
            // matching its key, and without re-keying the next object that
            // asked for the old key would receive the mesh with the new texture.
            bool rekey(int index, const std::string& newKey);

            // Forces the destruction of everything alive, ignoring refcounts. ONLY
            // from Renderer::shutdown, where nobody is left drawing.
            void destroyAll(const Destroyer& destroy);

            SharedGpuMesh*       get(int index);
            const SharedGpuMesh* get(int index) const;

            // The key under which the entry `index` is indexed, or an
            // EMPTY string if it is not alive (or the index is out of range).
            //
            // It is requested by the hot material swap: makeSharedMeshKey
            // puts the vertex count and the index count in the clear as the
            // first two fields, so comparing that prefix against that of
            // the new key tells whether the geometry is still the same without
            // rehashing the mesh. Whoever mutates an entry in place needs to
            // know it: mutating without re-uploading geometry and re-keying afterwards would leave
            // the entry advertising itself with a key that does not describe what it
            // holds, and because of the dedup that is picked up by the NEXT one that asks for it.
            //
            // The reference is valid until the next acquire/release/rekey, which
            // can move the entries vector.
            const std::string& keyOf(int index) const;

            // 0 if the index is not alive.
            int    refCount(int index) const;
            size_t liveCount() const;
            // Live indices, in increasing order. Used by createDescriptorSets to
            // walk entries instead of objects.
            std::vector<int> liveIndices() const;

        private:
            struct Entry
            {
                SharedGpuMesh gpu;
                std::string   key;
                int           refs = 0;
                bool          live = false;
            };

            std::vector<Entry>                   m_entries;
            std::unordered_map<std::string, int> m_byKey;
            std::vector<int>                     m_freeSlots;
    };
}
