// Test headless del compartido de recursos GPU (sin GUI, sin device Vulkan).
// SharedGpuMeshCache no llama a Vulkan: crear y destruir son callbacks del
// caller, y eso es justo lo que permite ejercitar aquí la parte que puede
// romperse de verdad — la clave de contenido y el refcount — con handles
// falsos pero DISTINTOS por creación. Plain main + asserts, sin framework,
// coherente con frustum_tests.cpp.
//
// Lo que se prueba no es "¿comparte?" sino las dos formas de romperlo:
// compartir de MÁS (dos mallas distintas acabando en el mismo buffer) y
// liberar de MÁS (borrar un objeto y llevarse por delante los recursos que
// sus gemelos siguen dibujando).
#include "DonTopo/Renderer/SharedGpuMesh.h"
#include "DonTopo/Renderer/Mesh.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Handles falsos con identidad propia: cada creación reparte números nuevos, de
// modo que "los dos objetos tienen el mismo VkBuffer" solo puede pasar si la
// entrada NO se ha creado dos veces. Afirmar tamaños iguales no distinguiría
// nada; afirmar handles iguales sí.
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

    // Cubo mínimo pero con contenido real: dos triángulos con posiciones y UVs
    // distintas entre sí, para que cambiar un solo vértice cambie el hash.
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

// N objetos con la misma malla+material resuelven a la MISMA entrada, y el
// creador se invoca una sola vez. Sin esto, agrupar draws por "mismo vertex
// buffer" agruparía cero objetos.
static void test_objetos_identicos_comparten_handles()
{
    FakeGpu gpu;
    SharedGpuMeshCache cache;

    const Mesh a = makeMesh("Cube");
    // Mismo contenido, distinto nombre y distinto fichero de origen: ninguna de
    // las dos cosas sube un byte a la GPU, así que no deben impedir compartir.
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

    // Un tercero más, por el caso de N>2 que es el que motiva la feature.
    const int ic = cache.acquire(makeSharedMeshKey(makeMesh("Cube (2)")), gpu.creator());
    CHECK(ic == ia);
    CHECK(gpu.creations == 1);
    CHECK(cache.refCount(ia) == 3);
}

// El otro modo de fallo: compartir de más. Geometría distinta, material
// distinto o factores PBR distintos tienen que dar entradas separadas.
static void test_mallas_distintas_no_comparten()
{
    FakeGpu gpu;
    SharedGpuMeshCache cache;

    const Mesh base = makeMesh("A");

    Mesh otraGeometria = makeMesh("B", /*x=*/5.0f);
    Mesh otraTextura   = makeMesh("C");
    otraTextura.material.texturePath = "assets/wood.png";
    Mesh otroPbr       = makeMesh("D");
    otroPbr.material.roughness = 0.1f;
    Mesh menosIndices  = makeMesh("E");
    menosIndices.indices.pop_back();
    Mesh conEmbebida   = makeMesh("F");
    conEmbebida.material.embeddedNormalMap = {1, 2, 3, 4};

    const int i0 = cache.acquire(makeSharedMeshKey(base),          gpu.creator());
    const int i1 = cache.acquire(makeSharedMeshKey(otraGeometria), gpu.creator());
    const int i2 = cache.acquire(makeSharedMeshKey(otraTextura),   gpu.creator());
    const int i3 = cache.acquire(makeSharedMeshKey(otroPbr),       gpu.creator());
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

// Borrar uno de N objetos idénticos NO puede destruir nada: los N-1 que quedan
// siguen dibujando esos mismos handles. Solo el último los suelta.
static void test_borrar_uno_deja_vivos_los_demas()
{
    FakeGpu gpu;
    SharedGpuMeshCache cache;

    const int i0 = cache.acquire(makeSharedMeshKey(makeMesh("Cube")),     gpu.creator());
    const int i1 = cache.acquire(makeSharedMeshKey(makeMesh("Cube (1)")), gpu.creator());
    const int i2 = cache.acquire(makeSharedMeshKey(makeMesh("Cube (2)")), gpu.creator());
    CHECK(i0 == i1 && i1 == i2);

    // Copia de los handles ANTES de soltar nada: es contra estos contra los que
    // se comprueba que el superviviente sigue siendo el mismo recurso.
    const SharedGpuMesh original = *cache.get(i0);

    cache.release(i0, gpu.destroyer());
    CHECK(gpu.destructions == 0);
    CHECK(cache.refCount(i1) == 2);
    const SharedGpuMesh* survivor = cache.get(i1);
    CHECK(survivor != nullptr);
    // Los CHECK van dentro del if a propósito: con el refcount roto survivor es
    // nullptr y el test tiene que FALLAR, no petar — un crash aquí no
    // distinguiría un bug del compartido de un bug del propio test.
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

    // Ahora sí: cae el último holder y los recursos se destruyen una vez.
    cache.release(i2, gpu.destroyer());
    CHECK(gpu.destructions == 1);
    CHECK(cache.liveCount() == 0);
    CHECK(cache.get(i2) == nullptr);
    CHECK(cache.refCount(i2) == 0);
    CHECK(gpu.destroyed.size() == 1);
    if (!gpu.destroyed.empty()) CHECK(sameHandles(gpu.destroyed[0], original));

    // Releases de más (el editor puede pedir borrar dos veces el mismo índice)
    // no vuelven a destruir.
    cache.release(i2, gpu.destroyer());
    CHECK(gpu.destructions == 1);
}

// El slot liberado se reutiliza en cuanto entra otra malla, pero la destrucción
// real va diferida varios frames: si release entregara la entrada por
// referencia en vez de por copia, el destructor acabaría cerrando los handles
// del inquilino NUEVO.
static void test_reutilizar_slot_no_pisa_el_snapshot()
{
    FakeGpu gpu;
    SharedGpuMeshCache cache;

    const int viejo = cache.acquire(makeSharedMeshKey(makeMesh("A")), gpu.creator());
    const SharedGpuMesh handlesViejos = *cache.get(viejo);

    cache.release(viejo, gpu.destroyer());
    CHECK(gpu.destroyed.size() == 1);

    const int nuevo = cache.acquire(makeSharedMeshKey(makeMesh("B", /*x=*/9.0f)), gpu.creator());
    CHECK(nuevo == viejo);                       // slot reciclado
    CHECK(gpu.creations == 2);
    CHECK(cache.get(nuevo) != nullptr);
    if (cache.get(nuevo))
        CHECK(cache.get(nuevo)->vertexBuffer != handlesViejos.vertexBuffer);
    // El snapshot que se encoló sigue apuntando a los recursos viejos.
    if (!gpu.destroyed.empty()) CHECK(sameHandles(gpu.destroyed[0], handlesViejos));
}

// destroyAll (shutdown) se lleva todo aunque queden holders, y una sola vez por
// entrada, no una por objeto.
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

// liveIndices es lo que recorre createDescriptorSets: tiene que dar una entrada
// por recurso, no una por objeto, o se alojarían sets de más y se perderían.
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

int main()
{
    test_objetos_identicos_comparten_handles();
    test_mallas_distintas_no_comparten();
    test_borrar_uno_deja_vivos_los_demas();
    test_reutilizar_slot_no_pisa_el_snapshot();
    test_destroy_all_libera_cada_entrada_una_vez();
    test_live_indices_son_entradas_no_objetos();

    if (g_failures == 0) std::printf("shared_gpu_mesh_tests: OK\n");
    else                 std::printf("shared_gpu_mesh_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
