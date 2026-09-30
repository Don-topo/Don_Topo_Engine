// Headless test of GPU resource sharing (no GUI, no Vulkan device).
// SharedGpuMeshCache does not call Vulkan: create and destroy are callbacks of the
// caller, and that is exactly what allows exercising here the part that can
// really break (the content key and the refcount) with fake handles but
// DISTINCT per creation. Plain main + asserts, no framework,
// consistent with frustum_tests.cpp.
//
// What is tested is not "does it share?" but the two ways to break it:
// sharing TOO MUCH (two different meshes ending up in the same buffer) and
// releasing TOO MUCH (deleting one object and taking with it the resources
// that its twins are still drawing).
#include "DonTopo/Renderer/SharedGpuMesh.h"
#include "DonTopo/Renderer/Mesh.h"

#include "DonTopo/Core/ImportSettings.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Fake handles with their own identity: each creation hands out new numbers, so
// that "both objects have the same VkBuffer" can only happen if the
// entry has NOT been created twice. Asserting equal sizes would not distinguish
// anything; asserting equal handles does.
namespace
{
    struct FakeGpu
    {
        int  creations = 0;
        int  destructions = 0;
        std::vector<SharedGpuMesh> destroyed;
        uint64_t next = 1;

        template <class H> H mint() { return (H)(uintptr_t)(next++); }

        void create(SharedGpuMesh& g)
        {
            ++creations;
            g.vertexBuffer      = mint<VkBuffer>();
            g.vertexMemory      = mint<VkDeviceMemory>();
            g.indexBuffer       = mint<VkBuffer>();
            g.indexMemory       = mint<VkDeviceMemory>();
            g.textureImage      = mint<VkImage>();
            g.textureView       = mint<VkImageView>();
            g.sampler           = mint<VkSampler>();
            g.normalImage       = mint<VkImage>();
            g.ormImage          = mint<VkImage>();
            g.descriptorSets[0] = mint<VkDescriptorSet>();
            g.descriptorSets[1] = mint<VkDescriptorSet>();
        }

        void destroy(const SharedGpuMesh& g)
        {
            ++destructions;
            destroyed.push_back(g);
        }

        SharedGpuMeshCache::Creator   creator()   { return [this](SharedGpuMesh& g) { create(g); }; }
        SharedGpuMeshCache::Destroyer destroyer() { return [this](const SharedGpuMesh& g) { destroy(g); }; }
    };

    // Minimal cube but with real content: two triangles with positions and UVs
    // different from each other, so that changing a single vertex changes the hash.
    Mesh makeMesh(const char* name, float x = 0.0f)
    {
        Mesh m;
        m.name = name;
        for (int i = 0; i < 4; ++i)
        {
            Vertex v{};
            v.pos     = glm::vec3(x + (float)i, (float)(i * 2), -1.0f);
            v.color   = glm::vec3(1.0f, 0.5f, 0.25f);
            v.uv      = glm::vec2((float)i * 0.1f, 0.75f);
            v.normal  = glm::vec3(0.0f, 1.0f, 0.0f);
            v.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
            m.vertices.push_back(v);
        }
        m.indices = {0, 1, 2, 2, 3, 0};
        m.material.texturePath = "assets/rock.png";
        m.material.metallic    = 0.25f;
        m.material.roughness   = 0.75f;
        return m;
    }

    bool sameHandles(const SharedGpuMesh& a, const SharedGpuMesh& b)
    {
        return a.vertexBuffer      == b.vertexBuffer
            && a.vertexMemory      == b.vertexMemory
            && a.indexBuffer       == b.indexBuffer
            && a.indexMemory       == b.indexMemory
            && a.textureImage      == b.textureImage
            && a.normalImage       == b.normalImage
            && a.ormImage          == b.ormImage
            && a.textureView       == b.textureView
            && a.sampler           == b.sampler
            && a.descriptorSets[0] == b.descriptorSets[0]
            && a.descriptorSets[1] == b.descriptorSets[1];
    }
}

// N objects with the same mesh+material resolve to the SAME entry, and the
// creator is invoked only once. Without this, grouping draws by "same vertex
// buffer" would group zero objects.
static void test_objetos_identicos_comparten_handles()
{
    FakeGpu gpu;
    SharedGpuMeshCache cache;

    const Mesh a = makeMesh("Cube");
    // Same content, different name and different source file: neither
    // uploads a byte to the GPU, so they must not prevent sharing.
    Mesh b = makeMesh("Cube (1)");
    b.name       = "otro nombre";
    b.sourcePath = "otra/ruta.fbx";

    const int ia = cache.acquire(makeSharedMeshKey(a), gpu.creator());
    const int ib = cache.acquire(makeSharedMeshKey(b), gpu.creator());

    CHECK(ia == ib);
    CHECK(gpu.creations == 1);
    CHECK(cache.liveCount() == 1);
    CHECK(cache.refCount(ia) == 2);

    const SharedGpuMesh* ga = cache.get(ia);
    const SharedGpuMesh* gb = cache.get(ib);
    CHECK(ga != nullptr && gb != nullptr);
    CHECK(ga == gb);
    if (ga && gb)
    {
        CHECK(ga->vertexBuffer != VK_NULL_HANDLE);
        CHECK(sameHandles(*ga, *gb));
    }

    // A third one, for the N>2 case that is the one that motivates the feature.
    const int ic = cache.acquire(makeSharedMeshKey(makeMesh("Cube (2)")), gpu.creator());
    CHECK(ic == ia);
    CHECK(gpu.creations == 1);
    CHECK(cache.refCount(ia) == 3);
}

// The other failure mode: sharing too much. Different geometry or different
// material have to give separate entries. The PBR factors no longer do: see
// test_factores_distintos_si_comparten right below.
static void test_mallas_distintas_no_comparten()
{
    FakeGpu gpu;
    SharedGpuMeshCache cache;

    const Mesh base = makeMesh("A");

    Mesh otraGeometria = makeMesh("B", /*x=*/5.0f);
    Mesh otraTextura   = makeMesh("C");
    otraTextura.material.texturePath = "assets/wood.png";
    Mesh otroOrm       = makeMesh("D");
    otroOrm.material.metallicRoughnessPath = "assets/rusty_orm.png";
    Mesh menosIndices  = makeMesh("E");
    menosIndices.indices.pop_back();
    Mesh conEmbebida   = makeMesh("F");
    conEmbebida.material.embeddedNormalMap = {1, 2, 3, 4};

    const int i0 = cache.acquire(makeSharedMeshKey(base),          gpu.creator());
    const int i1 = cache.acquire(makeSharedMeshKey(otraGeometria), gpu.creator());
    const int i2 = cache.acquire(makeSharedMeshKey(otraTextura),   gpu.creator());
    const int i3 = cache.acquire(makeSharedMeshKey(otroOrm),       gpu.creator());
    const int i4 = cache.acquire(makeSharedMeshKey(menosIndices),  gpu.creator());
    const int i5 = cache.acquire(makeSharedMeshKey(conEmbebida),   gpu.creator());

    CHECK(gpu.creations == 6);
    CHECK(cache.liveCount() == 6);

    const int idx[] = {i0, i1, i2, i3, i4, i5};
    for (int a = 0; a < 6; ++a)
        for (int b = a + 1; b < 6; ++b)
        {
            CHECK(idx[a] != idx[b]);
            const SharedGpuMesh* ga = cache.get(idx[a]);
            const SharedGpuMesh* gb = cache.get(idx[b]);
            CHECK(ga != nullptr && gb != nullptr);
            if (!ga || !gb) continue;
            CHECK(ga->vertexBuffer != gb->vertexBuffer);
            CHECK(ga->textureImage != gb->textureImage);
        }
}

// The reverse, and the reason for live dragging: two equal meshes that
// only differ in the PBR factors now SHARE an entry. While the
// factors were in the key, these were two copies of the same geometry and
// the same textures in VRAM, and moving a slider forced re-keying the object
// (redoing its resources) instead of writing two floats.
//
// What CANNOT happen is the ORM map sneaking in: its path names a real
// texture and still splits the entry (the test above covers it, with
// otroOrm).
static void test_factores_distintos_si_comparten()
{
    FakeGpu gpu;
    SharedGpuMeshCache cache;

    Mesh mate = makeMesh("Cube");
    mate.material.metallic  = 0.0f;
    mate.material.roughness = 0.9f;

    Mesh metalico = makeMesh("Cube (1)");
    metalico.material.metallic  = 1.0f;
    metalico.material.roughness = 0.1f;

    const int i0 = cache.acquire(makeSharedMeshKey(mate),     gpu.creator());
    const int i1 = cache.acquire(makeSharedMeshKey(metalico), gpu.creator());

    CHECK(i0 == i1);
    CHECK(gpu.creations == 1);
    CHECK(cache.refCount(i0) == 2);
}

// Deleting one of N identical objects CANNOT destroy anything: the N-1 that remain
// keep drawing those same handles. Only the last one releases them.
static void test_borrar_uno_deja_vivos_los_demas()
{
    FakeGpu gpu;
    SharedGpuMeshCache cache;

    const int i0 = cache.acquire(makeSharedMeshKey(makeMesh("Cube")),     gpu.creator());
    const int i1 = cache.acquire(makeSharedMeshKey(makeMesh("Cube (1)")), gpu.creator());
    const int i2 = cache.acquire(makeSharedMeshKey(makeMesh("Cube (2)")), gpu.creator());
    CHECK(i0 == i1 && i1 == i2);

    // Copy of the handles BEFORE releasing anything: it is against these that
    // it is checked that the survivor is still the same resource.
    const SharedGpuMesh original = *cache.get(i0);

    cache.release(i0, gpu.destroyer());
    CHECK(gpu.destructions == 0);
    CHECK(cache.refCount(i1) == 2);
    const SharedGpuMesh* survivor = cache.get(i1);
    CHECK(survivor != nullptr);
    // The CHECKs go inside the if on purpose: with the refcount broken survivor is
    // nullptr and the test has to FAIL, not crash; a crash here would not
    // distinguish a bug in the sharing from a bug in the test itself.
    if (survivor)
    {
        CHECK(survivor->vertexBuffer != VK_NULL_HANDLE);
        CHECK(sameHandles(*survivor, original));
    }

    cache.release(i1, gpu.destroyer());
    CHECK(gpu.destructions == 0);
    CHECK(cache.refCount(i2) == 1);
    const SharedGpuMesh* ultimo = cache.get(i2);
    CHECK(ultimo != nullptr);
    if (ultimo) CHECK(sameHandles(*ultimo, original));

    // Now it does: the last holder falls and the resources are destroyed once.
    cache.release(i2, gpu.destroyer());
    CHECK(gpu.destructions == 1);
    CHECK(cache.liveCount() == 0);
    CHECK(cache.get(i2) == nullptr);
    CHECK(cache.refCount(i2) == 0);
    CHECK(gpu.destroyed.size() == 1);
    if (!gpu.destroyed.empty()) CHECK(sameHandles(gpu.destroyed[0], original));

    // Extra releases (the editor may ask to delete the same index twice)
    // do not destroy again.
    cache.release(i2, gpu.destroyer());
    CHECK(gpu.destructions == 1);
}

// The freed slot is reused as soon as another mesh comes in, but the real
// destruction is deferred several frames: if release handed over the entry by
// reference instead of by copy, the destructor would end up closing the handles
// of the NEW tenant.
static void test_reutilizar_slot_no_pisa_el_snapshot()
{
    FakeGpu gpu;
    SharedGpuMeshCache cache;

    const int viejo = cache.acquire(makeSharedMeshKey(makeMesh("A")), gpu.creator());
    const SharedGpuMesh handlesViejos = *cache.get(viejo);

    cache.release(viejo, gpu.destroyer());
    CHECK(gpu.destroyed.size() == 1);

    const int nuevo = cache.acquire(makeSharedMeshKey(makeMesh("B", /*x=*/9.0f)), gpu.creator());
    CHECK(nuevo == viejo);                       // recycled slot
    CHECK(gpu.creations == 2);
    CHECK(cache.get(nuevo) != nullptr);
    if (cache.get(nuevo))
        CHECK(cache.get(nuevo)->vertexBuffer != handlesViejos.vertexBuffer);
    // The snapshot that was enqueued still points to the old resources.
    if (!gpu.destroyed.empty()) CHECK(sameHandles(gpu.destroyed[0], handlesViejos));
}

// destroyAll (shutdown) takes everything even if holders remain, and only once per
// entry, not once per object.
static void test_destroy_all_libera_cada_entrada_una_vez()
{
    FakeGpu gpu;
    SharedGpuMeshCache cache;

    cache.acquire(makeSharedMeshKey(makeMesh("A")),            gpu.creator());
    cache.acquire(makeSharedMeshKey(makeMesh("A (1)")),        gpu.creator());
    cache.acquire(makeSharedMeshKey(makeMesh("B", 3.0f)),      gpu.creator());
    CHECK(cache.liveCount() == 2);

    cache.destroyAll(gpu.destroyer());
    CHECK(gpu.destructions == 2);
    CHECK(cache.liveCount() == 0);
    CHECK(cache.liveIndices().empty());
}

// liveIndices is what createDescriptorSets walks: it has to give one entry
// per resource, not one per object, or extra sets would be allocated and lost.
static void test_live_indices_son_entradas_no_objetos()
{
    FakeGpu gpu;
    SharedGpuMeshCache cache;

    for (int i = 0; i < 5; ++i)
        cache.acquire(makeSharedMeshKey(makeMesh("Cube")), gpu.creator());
    cache.acquire(makeSharedMeshKey(makeMesh("Otro", 7.0f)), gpu.creator());

    CHECK(cache.liveIndices().size() == 2);
    CHECK(gpu.creations == 2);
}

// After re-keying, the entry is found by the new key and the old one is left
// free for a different entry.
static void test_rekey_moves_the_entry()
{
    SharedGpuMeshCache cache;
    int creadas = 0;
    auto crear  = [&](SharedGpuMesh&) { ++creadas; };

    const int idx = cache.acquire("vieja", crear);
    CHECK(cache.rekey(idx, "nueva"));

    // The new key returns the SAME entry, without creating another.
    CHECK(cache.acquire("nueva", crear) == idx);
    CHECK(creadas == 1);
    // And the old one no longer finds it: it creates a different entry.
    CHECK(cache.acquire("vieja", crear) != idx);
    CHECK(creadas == 2);
}

// Re-keying to a key that already has ANOTHER entry is rejected: it would leave one of
// the two unreachable in the map, that is, a GPU resource leak.
static void test_rekey_rejects_collision()
{
    SharedGpuMeshCache cache;
    auto crear = [](SharedGpuMesh&) {};
    const int a = cache.acquire("a", crear);
    const int b = cache.acquire("b", crear);
    CHECK(a != b);
    CHECK(!cache.rekey(a, "b"));
    // And a's is still found by its usual key.
    CHECK(cache.acquire("a", crear) == a);
}

// Dead index: it does nothing and says so.
static void test_rekey_on_dead_index()
{
    SharedGpuMeshCache cache;
    CHECK(!cache.rekey(0, "loquesea"));
    CHECK(!cache.rekey(-1, "loquesea"));
}

// Re-keying to the key it already had is a no-op that returns true.
static void test_rekey_to_same_key()
{
    SharedGpuMeshCache cache;
    auto crear = [](SharedGpuMesh&) {};
    const int idx = cache.acquire("k", crear);
    CHECK(cache.rekey(idx, "k"));
    CHECK(cache.acquire("k", crear) == idx);
}

// The refcount is not touched: two owners before, two owners after.
static void test_rekey_preserves_refcount()
{
    SharedGpuMeshCache cache;
    auto crear = [](SharedGpuMesh&) {};
    const int idx = cache.acquire("k", crear);
    cache.acquire("k", crear);
    CHECK(cache.refCount(idx) == 2);
    CHECK(cache.rekey(idx, "otra"));
    CHECK(cache.refCount(idx) == 2);
}

// IN-RANGE index but whose slot was freed: it is the case that really justifies
// the `live` check (out-of-range indices already fall earlier, because of the
// vector bounds). It has to be rejected and, above all, it cannot leave in
// the map an entry pointing to a dead slot: a later acquire with that new
// key has to create a real entry, not recycle the dead
// index.
static void test_rekey_on_freed_slot_in_range()
{
    SharedGpuMeshCache cache;
    int creadas = 0;
    auto crear  = [&](SharedGpuMesh&) { ++creadas; };
    auto nada   = [](const SharedGpuMesh&) {};

    const int idx = cache.acquire("k", crear);
    CHECK(creadas == 1);
    cache.release(idx, nada);

    CHECK(!cache.rekey(idx, "otra"));

    // The map was not left pointing to the dead slot under "otra": acquire creates a new
    // entry, it does not return the recycled index.
    const int nuevo = cache.acquire("otra", crear);
    CHECK(creadas == 2);
    CHECK(cache.get(nuevo) != nullptr);
}

// keyOf is what rebuildStaticMesh reads before mutating an entry in place:
// it compares the geometry prefix of this key against that of the new key. If it
// returned the key of a dead slot (that is, the previous tenant's), that
// comparison would say "same geometry" about an entry that is no longer the one that
// was believed, and the dedup would hand out the wrong mesh. That is why what is asserted is
// not only that it gets the live one right, but that the dead one gives EMPTY.
static void test_key_of_entrada_viva_y_muerta()
{
    SharedGpuMeshCache cache;
    auto crear = [](SharedGpuMesh&) {};
    auto nada  = [](const SharedGpuMesh&) {};

    const int idx = cache.acquire("12|30|difusa.png", crear);
    CHECK(cache.keyOf(idx) == "12|30|difusa.png");

    // It follows the entry when it is re-keyed: it is what makes it usable
    // after a material change.
    CHECK(cache.rekey(idx, "12|30|otra.png"));
    CHECK(cache.keyOf(idx) == "12|30|otra.png");

    // Out of range above and below. This DOES discriminate: without the
    // bounds check, keyOf(-1) indexes the vector out of place.
    CHECK(cache.keyOf(idx + 1).empty());
    CHECK(cache.keyOf(-1).empty());

    // IN-range index whose slot was freed. Warning for whoever reads this believing
    // that it tests the `live` guard of keyOf: it does NOT test it. release leaves the
    // entry as Entry{}, that is, with the key already empty, so this would pass
    // just the same without that guard (checked by sabotaging it). What it asserts is
    // the pair (release cleans AND keyOf does not betray keys of dead entries),
    // which is the property the caller depends on.
    cache.release(idx, nada);
    CHECK(cache.keyOf(idx).empty());

    // And the case that really has an edge: the slot goes back to the freelist and
    // another entry takes it over. keyOf has to give the key of the NEW TENANT.
    // Whoever kept the earlier key and compared it against this one (which is
    // exactly what the fast path of rebuildStaticMesh does) has to
    // see that it has changed, not the dead one's key.
    const int reciclado = cache.acquire("99|7|otra_malla.png", crear);
    CHECK(reciclado == idx);
    CHECK(cache.keyOf(reciclado) == "99|7|otra_malla.png");
}

// Review Focus 3: two identical meshes with the same texture file but with
// different import settings do NOT share an entry in VRAM.
static void test_key_changes_with_texture_import_settings()
{
    std::error_code ec;
    const std::filesystem::path d = std::filesystem::temp_directory_path(ec) / "dt_meshkey_import_test";
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);

    Mesh a = makeMesh("A");
    a.material.texturePath = (d / "t.png").string();
    const std::string sinSidecar = makeSharedMeshKey(a);

    TextureImportSettings s;
    s.mipmaps = true;
    std::string err;
    CHECK(saveTextureImportSettings(d / "t.png", s, &err));
    const std::string conMips = makeSharedMeshKey(a);
    CHECK(conMips != sinSidecar);

    // The geometry prefix (the first two fields) is not altered: rebuildStaticMesh
    // uses it to know whether the geometry is still the same.
    auto prefijo = [](const std::string& k) { return k.substr(0, k.find('|', k.find('|') + 1)); };
    CHECK(prefijo(conMips) == prefijo(sinSidecar));

    // With no sidecar of any kind, the key is the usual one.
    Mesh b = makeMesh("A");
    b.material.texturePath = (d / "otra.png").string();
    CHECK(makeSharedMeshKey(b).find("|ts") == std::string::npos);
    std::filesystem::remove_all(d, ec);
}

int main()
{
    test_key_changes_with_texture_import_settings();
    test_objetos_identicos_comparten_handles();
    test_mallas_distintas_no_comparten();
    test_factores_distintos_si_comparten();
    test_borrar_uno_deja_vivos_los_demas();
    test_reutilizar_slot_no_pisa_el_snapshot();
    test_destroy_all_libera_cada_entrada_una_vez();
    test_live_indices_son_entradas_no_objetos();
    test_rekey_moves_the_entry();
    test_rekey_rejects_collision();
    test_rekey_on_dead_index();
    test_rekey_to_same_key();
    test_rekey_preserves_refcount();
    test_rekey_on_freed_slot_in_range();
    test_key_of_entrada_viva_y_muerta();

    if (g_failures == 0) std::printf("shared_gpu_mesh_tests: OK\n");
    else                 std::printf("shared_gpu_mesh_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
