// Test headless de las guardas de visibilidad (sin GUI, sin device Vulkan).
// Visibility::objectVisible y gatherCandidates no llaman a Vulkan: la caché de
// mallas reparte handles por callback, así que la decisión completa —checkbox,
// entrada borrada, upload en vuelo y frustum— se puede ejercitar aquí. Plain
// main + asserts, sin framework, coherente con frustum_tests.cpp.
//
// Lo que se prueba no es "¿culea?" —eso ya lo cubre frustum_tests— sino las
// cuatro guardas que estaban COPIADAS en los cuatro pases del backend. Cada
// caso monta el estado que hace fallar a una sola de ellas: si alguna
// desaparece del helper, exactamente un caso se pone rojo y dice cuál.
#include "DonTopo/Renderer/VisibleSet.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Cámara en el origen mirando hacia -Z, mismo convenio que el motor.
static Culling::Frustum camaraEnOrigen()
{
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 1.0f, 1000.0f);
    proj[1][1] *= -1.0f; // Y-flip de Vulkan
    return Culling::frustumFromViewProj(proj * view);
}

static glm::mat4 en(float x, float y, float z)
{
    return glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z));
}

// Escena de mentira: N objetos, cada uno con su entrada en la caché. Las mallas
// y los objetos se crean TODOS de golpe y solo después se piden punteros: los
// dos contenedores son vectores, así que una creación tardía reubicaría la
// memoria y dejaría colgado cualquier puntero pedido antes.
struct Escena
{
    SharedGpuMeshCache        meshes;
    std::vector<RenderObject> objects;

    void anade(const char* key, const glm::mat4& xform)
    {
        RenderObject obj;
        // Caja acotada de semilado 1, como cualquier malla real: el AABB va en
        // local y lo coloca el transform, igual que en el Renderer.
        obj.sharedIndex = meshes.acquire(key, [](SharedGpuMesh& g) {
            g.aabbMin   = glm::vec3(-1.0f);
            g.aabbMax   = glm::vec3( 1.0f);
            g.hasBounds = true;
        });
        obj.transform = xform;
        objects.push_back(obj);
    }
};

static void test_guardas_una_a_una()
{
    const Culling::Frustum cam = camaraEnOrigen();

    Escena e;
    e.anade("a", en(0.0f, 0.0f, -50.0f));
    e.anade("b", en(0.0f, 0.0f, -50.0f));

    RenderObject& obj  = e.objects[0];
    SharedGpuMesh* gpu = e.meshes.get(obj.sharedIndex);

    // Caso base: delante de la cámara, subido y con el checkbox puesto.
    CHECK(Visibility::objectVisible(obj, gpu, 0, cam));

    // 1. Checkbox "Visible" del componente Mesh.
    obj.meshVisible = false;
    CHECK(!Visibility::objectVisible(obj, gpu, 0, cam));
    obj.meshVisible = true;

    // 2. Upload en vuelo: el ticket del objeto va por delante del último
    //    completado. Sus texturas siguen en TRANSFER_DST_OPTIMAL y samplearlas
    //    sería leer basura.
    gpu->uploadTicket = 7;
    CHECK(!Visibility::objectVisible(obj, gpu, 6, cam));
    // Frontera: el ticket IGUAL al último completado ya está subido. Un `<` en
    // vez de `<=` haría parpadear un frame a cada malla que se importa.
    CHECK(Visibility::objectVisible(obj, gpu, 7, cam));
    CHECK(Visibility::objectVisible(obj, gpu, 8, cam));
    gpu->uploadTicket = 0;

    // 3. Frustum: bien detrás de la cámara, lo bastante lejos como para que
    //    ninguna holgura conservadora lo salve.
    obj.transform = en(0.0f, 0.0f, 400.0f);
    CHECK(!Visibility::objectVisible(obj, gpu, 0, cam));

    // 4. Sin AABB (mesh vacío) no se puede acotar: pasa aunque esté detrás.
    //    Descartarlo sería culear de MÁS, que es el fallo que se ve en pantalla.
    gpu->hasBounds = false;
    CHECK(Visibility::objectVisible(obj, gpu, 0, cam));

    // 5. Entrada borrada desde el editor: el índice deja de estar vivo y la
    //    caché devuelve nullptr. Se comprueba con la caché de verdad y no
    //    pasando nullptr a mano, que es lo que hace el Renderer. Va la última
    //    porque libera un slot que la siguiente creación reutilizaría.
    RenderObject& segundo = e.objects[1];
    e.meshes.release(segundo.sharedIndex, [](const SharedGpuMesh&) {});
    CHECK(e.meshes.get(segundo.sharedIndex) == nullptr);
    CHECK(!Visibility::objectVisible(segundo, e.meshes.get(segundo.sharedIndex), 0, cam));
}

// El frustum es parámetro y no un miembro precisamente por esto: el pase de
// sombras evalúa el MISMO objeto con el de su cascada. Un objeto que la cámara
// no ve puede seguir proyectando sombra sobre lo que sí se ve, así que las dos
// respuestas tienen que poder diferir.
static void test_dos_frustums_mismo_objeto()
{
    const Culling::Frustum cam = camaraEnOrigen();

    // Volumen de una luz que mira hacia -X desde la derecha: cubre lo que hay
    // al lado de la cámara, donde el frustum de la cámara no llega.
    const glm::mat4 luzView = glm::lookAt(glm::vec3(200.0f, 0.0f, 0.0f), glm::vec3(0.0f),
                                          glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 luzProj = glm::orthoRH_ZO(-100.0f, 100.0f, -100.0f, 100.0f, 1.0f, 400.0f);
    const Culling::Frustum luz = Culling::frustumFromViewProj(luzProj * luzView);

    Escena e;
    e.anade("a", en(-60.0f, 0.0f, 0.0f));

    const RenderObject&  obj = e.objects[0];
    const SharedGpuMesh* gpu = e.meshes.get(obj.sharedIndex);

    CHECK(!Visibility::objectVisible(obj, gpu, 0, cam));
    CHECK(Visibility::objectVisible(obj, gpu, 0, luz));
}

static void test_gather()
{
    const Culling::Frustum cam = camaraEnOrigen();

    Escena e;
    e.anade("a", en(0.0f, 0.0f, -50.0f));  // dentro
    e.anade("b", en(0.0f, 0.0f, 400.0f));  // detrás de la cámara
    e.objects[0].ssrStrength = 0.75f;
    e.objects[1].ssrStrength = 0.5f;

    std::vector<Batching::BatchCandidate> out;

    // Con basura previa: gatherCandidates limpia. Si no lo hiciera, el pase
    // dibujaría también los candidatos del pase anterior.
    out.push_back({ 99, true, nullptr, 0.0f });

    Visibility::gatherCandidates(e.objects, e.meshes, 0, cam, /*ssrEnabled*/ true, out);

    // Los invisibles TAMBIÉN entran, con visible = false: el agrupado los
    // salta, pero el panel Performance los cuenta como culleados. Devolver solo
    // los visibles dejaría ese contador a cero.
    CHECK(out.size() == 2);
    CHECK(out[0].visible);
    CHECK(!out[1].visible);
    // Orden estable y de los objetos, no del agrupado.
    CHECK(out[0].sharedIndex == e.objects[0].sharedIndex);
    CHECK(out[1].sharedIndex == e.objects[1].sharedIndex);
    // El transform va por PUNTERO al objeto: buildInstanceBatches lo copia al
    // buffer más tarde, así que apuntar a un temporal escribiría basura.
    CHECK(out[0].transform == &e.objects[0].transform);
    CHECK(out[1].transform == &e.objects[1].transform);
    CHECK(out[0].ssr == 0.75f);
    CHECK(out[1].ssr == 0.5f);

    // Los pases que no pintan color lo dejan a 0 aunque el objeto tenga fuerza:
    // el SSR entra en la clave del agrupado, y con un único valor salen menos
    // draws y el mapa resultante es idéntico.
    Visibility::gatherCandidates(e.objects, e.meshes, 0, cam, /*ssrEnabled*/ false, out);
    CHECK(out.size() == 2);
    CHECK(out[0].ssr == 0.0f);
    CHECK(out[1].ssr == 0.0f);
    // Y la visibilidad no depende del SSR.
    CHECK(out[0].visible);
    CHECK(!out[1].visible);
}

int main()
{
    test_guardas_una_a_una();
    test_dos_frustums_mismo_objeto();
    test_gather();

    if (g_failures == 0) std::printf("visible_set_tests: OK\n");
    else                 std::printf("visible_set_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
