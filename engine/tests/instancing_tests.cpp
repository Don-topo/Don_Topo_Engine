// Test headless del agrupado por instancing (sin GUI, sin Vulkan).
// Renderer::buildInstanceBatches es estática justo para poder ejercitarla sin un
// Renderer inicializado, igual que frustumFromViewProj. Plain main + asserts,
// sin framework — coherente con frustum_tests.cpp.
//
// Lo que se prueba no es "¿agrupa?" sino tres cosas que fallan en silencio: que
// cada instancia acabe en el slot que su draw va a leer (un firstInstance mal
// puesto no da error en ningún sitio: pinta el objeto equivocado en el sitio
// equivocado), que lo invisible NO gaste slot, y que sin capacidad se trunque en
// vez de escribir fuera del SSBO. Por eso cada transform lleva una traslación
// única y se comprueba la matriz slot a slot, no solo los contadores.
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/InstanceBuffers.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Traslación en X: identifica el objeto sin ambigüedad y no es el valor neutro,
// así que un slot que nadie escribe se distingue de uno bien escrito.
static glm::mat4 markerAt(float x)
{
    return glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f));
}

static float markerOf(const glm::mat4& m) { return m[3][0]; }

// Relleno reconocible del buffer de salida: cualquier slot que se quede sin
// escribir sale como -999 y no como una identidad que podría colarse.
static constexpr float kUnwritten = -999.0f;

static std::vector<glm::mat4> makeOut(size_t slots)
{
    return std::vector<glm::mat4>(slots, markerAt(kUnwritten));
}

// N objetos de la misma entrada compartida = UN draw con N instancias, y los
// transforms en el orden en que venían.
static void test_misma_entrada_un_solo_grupo()
{
    std::vector<glm::mat4> tf = { markerAt(10.0f), markerAt(20.0f), markerAt(30.0f) };
    std::vector<Renderer::BatchCandidate> cands;
    for (auto& m : tf) cands.push_back({ 7, true, &m });

    std::vector<glm::mat4> out = makeOut(8);
    std::vector<Renderer::InstanceBatch> batches;
    const uint32_t written = Renderer::buildInstanceBatches(cands.data(), cands.size(),
        out.data(), (uint32_t)out.size(), 0, batches);

    CHECK(written == 3);
    CHECK(batches.size() == 1);
    if (batches.size() == 1)
    {
        CHECK(batches[0].sharedIndex   == 7);
        CHECK(batches[0].firstInstance == 0);
        CHECK(batches[0].instanceCount == 3);
    }
    CHECK(markerOf(out[0]) == 10.0f);
    CHECK(markerOf(out[1]) == 20.0f);
    CHECK(markerOf(out[2]) == 30.0f);
    // Nada escrito más allá de lo que se dijo.
    CHECK(markerOf(out[3]) == kUnwritten);
}

// Entradas intercaladas: cada grupo tiene que quedar CONTIGUO (un draw
// instanciado lee un rango, no una lista) y el orden de grupos es el de primera
// aparición, que es lo que hace el resultado estable entre frames.
static void test_mezcla_de_entradas_agrupa_contiguo()
{
    std::vector<glm::mat4> tf = {
        markerAt(1.0f),  // A
        markerAt(2.0f),  // B
        markerAt(3.0f),  // A
        markerAt(4.0f),  // C
        markerAt(5.0f),  // B
        markerAt(6.0f),  // A
    };
    const int shared[] = { 4, 9, 4, 2, 9, 4 };
    std::vector<Renderer::BatchCandidate> cands;
    for (int i = 0; i < 6; i++) cands.push_back({ shared[i], true, &tf[(size_t)i] });

    std::vector<glm::mat4> out = makeOut(8);
    std::vector<Renderer::InstanceBatch> batches;
    const uint32_t written = Renderer::buildInstanceBatches(cands.data(), cands.size(),
        out.data(), (uint32_t)out.size(), 0, batches);

    CHECK(written == 6);
    CHECK(batches.size() == 3);
    if (batches.size() == 3)
    {
        CHECK(batches[0].sharedIndex == 4); // primera aparición: A
        CHECK(batches[1].sharedIndex == 9); // luego B
        CHECK(batches[2].sharedIndex == 2); // y por último C
        CHECK(batches[0].firstInstance == 0); CHECK(batches[0].instanceCount == 3);
        CHECK(batches[1].firstInstance == 3); CHECK(batches[1].instanceCount == 2);
        CHECK(batches[2].firstInstance == 5); CHECK(batches[2].instanceCount == 1);
    }
    // A: 1,3,6 seguidos; B: 2,5; C: 4. Si el agrupado escribiera por orden de
    // candidato en vez de por grupo, estos slots saldrían mezclados.
    CHECK(markerOf(out[0]) == 1.0f);
    CHECK(markerOf(out[1]) == 3.0f);
    CHECK(markerOf(out[2]) == 6.0f);
    CHECK(markerOf(out[3]) == 2.0f);
    CHECK(markerOf(out[4]) == 5.0f);
    CHECK(markerOf(out[5]) == 4.0f);
}

// Lo que el pass ya descartó (culleado, upload en vuelo, entrada borrada) llega
// con visible = false: no debe gastar slot ni aparecer en ningún grupo. Un
// agrupado que ignore el flag pintaría objetos fuera de cámara y, en el caso de
// la entrada borrada, dereferenciaría una entrada muerta al buscar sus buffers.
static void test_no_visibles_excluidos()
{
    std::vector<glm::mat4> tf = {
        markerAt(1.0f),  // culleado
        markerAt(2.0f),  // visible
        markerAt(3.0f),  // en vuelo
        markerAt(4.0f),  // visible
        markerAt(5.0f),  // borrado desde el editor: sharedIndex -1
    };
    std::vector<Renderer::BatchCandidate> cands = {
        { 3, false, &tf[0] },
        { 3, true,  &tf[1] },
        { 3, false, &tf[2] },
        { 3, true,  &tf[3] },
        { -1, false, &tf[4] },
    };

    std::vector<glm::mat4> out = makeOut(8);
    std::vector<Renderer::InstanceBatch> batches;
    const uint32_t written = Renderer::buildInstanceBatches(cands.data(), cands.size(),
        out.data(), (uint32_t)out.size(), 0, batches);

    CHECK(written == 2);
    CHECK(batches.size() == 1);
    if (batches.size() == 1)
    {
        CHECK(batches[0].sharedIndex   == 3);
        CHECK(batches[0].instanceCount == 2);
    }
    CHECK(markerOf(out[0]) == 2.0f);
    CHECK(markerOf(out[1]) == 4.0f);
    CHECK(markerOf(out[2]) == kUnwritten);

    // Todo invisible: ni grupos ni escrituras. Es el caso de una escena cargando
    // (todos los uploads en vuelo), que antes del agrupado era simplemente
    // "ningún draw".
    for (auto& c : cands) c.visible = false;
    std::vector<glm::mat4> out2 = makeOut(4);
    CHECK(Renderer::buildInstanceBatches(cands.data(), cands.size(),
              out2.data(), (uint32_t)out2.size(), 0, batches) == 0);
    CHECK(batches.empty());
    CHECK(markerOf(out2[0]) == kUnwritten);
}

// Los dos passes comparten el SSBO: el de sombras escribe delante y el de la
// escena detrás, con firstInstanceBase = lo ya escrito. Los firstInstance tienen
// que ser ABSOLUTOS (índices dentro del buffer) mientras que las escrituras son
// RELATIVAS al puntero que se pasa, que ya viene desplazado.
static void test_base_de_first_instance()
{
    std::vector<glm::mat4> tf = { markerAt(11.0f), markerAt(22.0f), markerAt(33.0f) };
    std::vector<Renderer::BatchCandidate> cands = {
        { 1, true, &tf[0] },
        { 5, true, &tf[1] },
        { 1, true, &tf[2] },
    };

    // El buffer completo con 4 slots ya ocupados por el pass anterior; se pasa el
    // puntero desplazado, como hace el Renderer.
    std::vector<glm::mat4> full = makeOut(12);
    const uint32_t base = 4;
    for (uint32_t i = 0; i < base; i++) full[i] = markerAt(100.0f + (float)i);

    std::vector<Renderer::InstanceBatch> batches;
    const uint32_t written = Renderer::buildInstanceBatches(cands.data(), cands.size(),
        full.data() + base, (uint32_t)full.size() - base, base, batches);

    CHECK(written == 3);
    CHECK(batches.size() == 2);
    if (batches.size() == 2)
    {
        CHECK(batches[0].firstInstance == base);      // no 0
        CHECK(batches[0].instanceCount == 2);
        CHECK(batches[1].firstInstance == base + 2);
        CHECK(batches[1].instanceCount == 1);
    }
    // El rango del pass anterior sigue intacto.
    CHECK(markerOf(full[0]) == 100.0f);
    CHECK(markerOf(full[3]) == 103.0f);
    // Y lo nuevo empieza justo en base.
    CHECK(markerOf(full[base + 0]) == 11.0f);
    CHECK(markerOf(full[base + 1]) == 33.0f);
    CHECK(markerOf(full[base + 2]) == 22.0f);
}

// Más objetos que capacidad: se trunca por grupos y NO se escribe ni un slot
// fuera del rango prometido. Escribir fuera aquí es corromper memoria mapeada
// del dispositivo, que no da error en ninguna parte.
static void test_trunca_sin_desbordar()
{
    std::vector<glm::mat4> tf;
    for (int i = 0; i < 6; i++) tf.push_back(markerAt((float)(i + 1)));
    const int shared[] = { 1, 1, 1, 2, 2, 3 };
    std::vector<Renderer::BatchCandidate> cands;
    for (int i = 0; i < 6; i++) cands.push_back({ shared[i], true, &tf[(size_t)i] });

    // Capacidad 4 para 6 instancias: entra el grupo 1 (3) y solo una del grupo 2.
    std::vector<glm::mat4> out = makeOut(10);
    const uint32_t capacity = 4;
    std::vector<Renderer::InstanceBatch> batches;
    const uint32_t written = Renderer::buildInstanceBatches(cands.data(), cands.size(),
        out.data(), capacity, 0, batches);

    CHECK(written == capacity);
    // El grupo 3 no cabe: no puede llegar como un draw de 0 instancias.
    CHECK(batches.size() == 2);
    uint32_t sum = 0;
    for (const auto& b : batches)
    {
        CHECK(b.instanceCount > 0);
        CHECK(b.firstInstance + b.instanceCount <= capacity);
        sum += b.instanceCount;
    }
    CHECK(sum == capacity);
    // Ni un slot tocado más allá de la capacidad.
    for (size_t i = capacity; i < out.size(); i++)
        CHECK(markerOf(out[i]) == kUnwritten);
}


// --- Reparto dentro del SSBO de instancias -----------------------------------

// InstanceCursor es la mitad SIN Vulkan de InstanceBuffers, y por eso se puede
// afirmar aqui entera. Lo que protege es la guarda que antes estaba copiada a
// mano en tres sitios del Renderer: comprobar el tope, avanzar el cursor y hacer
// la aritmetica de punteros. Saltarse la comprobacion en cualquiera de los tres
// escribia fuera del buffer sin que nada avisara.
static void test_cursor_reparte_contiguo()
{
    glm::mat4 buffer[8];
    for (auto& m : buffer) m = markerAt(kUnwritten);

    InstanceCursor cur;
    cur.reset(buffer, 8);
    CHECK(cur.cursor() == 0);
    CHECK(cur.capacity() == 8);

    glm::mat4* a = cur.alloc(3);
    CHECK(a == buffer);          // el primero empieza en el principio
    CHECK(cur.cursor() == 3);

    glm::mat4* b = cur.alloc(2);
    CHECK(b == buffer + 3);      // el siguiente va DETRAS, sin pisar
    CHECK(cur.cursor() == 5);

    // Lo que se escribe por el puntero acaba en el slot que su draw va a leer.
    a[0] = markerAt(10.0f);
    b[0] = markerAt(20.0f);
    CHECK(markerOf(buffer[0]) == 10.0f);
    CHECK(markerOf(buffer[3]) == 20.0f);
}

// No cabe: nullptr y el cursor NO se mueve. Las dos mitades importan — devolver
// nullptr pero haber avanzado dejaria un hueco muerto en el buffer.
static void test_cursor_no_cabe_devuelve_nulo()
{
    glm::mat4 buffer[4];
    InstanceCursor cur;
    cur.reset(buffer, 4);

    CHECK(cur.alloc(3) != nullptr);
    CHECK(cur.cursor() == 3);

    CHECK(cur.alloc(2) == nullptr);   // pide 2, queda 1
    CHECK(cur.cursor() == 3);         // y no se ha movido

    CHECK(cur.alloc(1) != nullptr);   // el ultimo hueco si cabe
    CHECK(cur.cursor() == 4);
    CHECK(cur.alloc(1) == nullptr);   // lleno
    CHECK(cur.cursor() == 4);
}

// La suma cursor + n va en 64 bits: en 32 daria la vuelta y pasaria la
// comprobacion, que es la forma silenciosa de escribir fuera del buffer.
static void test_cursor_no_desborda_el_uint32()
{
    glm::mat4 buffer[4];
    InstanceCursor cur;
    cur.reset(buffer, 4);
    CHECK(cur.alloc(3) != nullptr);

    // 3 + 0xFFFFFFFF da la vuelta a 2, que "cabe" en 4 si la suma es de 32 bits.
    CHECK(cur.alloc(0xFFFFFFFFu) == nullptr);
    CHECK(cur.cursor() == 3);
}

// Un buffer sin mapear (el frame cuyo buffer aun no existe) no acepta nada, en
// vez de entregar un puntero nulo con desplazamiento.
static void test_cursor_sin_buffer_no_reparte()
{
    InstanceCursor cur;
    cur.reset(nullptr, 1024);         // capacidad mentida a proposito
    CHECK(cur.capacity() == 0);       // no se la cree
    CHECK(cur.alloc(1) == nullptr);
    CHECK(cur.rest().data == nullptr);
    CHECK(cur.rest().capacity == 0);
}

// rest() da la cola libre para quien no sabe cuanto va a escribir hasta que
// termina, y commit() la cierra. Es el camino del agrupado por lotes.
static void test_cursor_rest_y_commit()
{
    glm::mat4 buffer[10];
    InstanceCursor cur;
    cur.reset(buffer, 10);
    CHECK(cur.alloc(4) != nullptr);

    InstanceCursor::Span s = cur.rest();
    CHECK(s.data == buffer + 4);
    CHECK(s.capacity == 6);
    CHECK(s.base == 4);               // la base de los firstInstance del lote

    cur.commit(2);
    CHECK(cur.cursor() == 6);
    CHECK(cur.rest().base == 6);
    CHECK(cur.rest().capacity == 4);

    // Un commit que se pasa se recorta: mover el cursor fuera del buffer dejaria
    // al siguiente pase escribiendo en tierra de nadie.
    cur.commit(999);
    CHECK(cur.cursor() == 10);
    CHECK(cur.rest().data == nullptr);   // lleno: no queda cola
    CHECK(cur.rest().capacity == 0);
    CHECK(cur.alloc(1) == nullptr);
}

// El buffer se comparte entre los dos pases del frame (sombras primero, escena
// detras). reset() es lo que separa un frame del siguiente.
static void test_cursor_reset_entre_frames()
{
    glm::mat4 buffer[4];
    InstanceCursor cur;
    cur.reset(buffer, 4);
    CHECK(cur.alloc(4) != nullptr);
    CHECK(cur.alloc(1) == nullptr);   // agotado

    cur.reset(buffer, 4);             // frame nuevo
    CHECK(cur.cursor() == 0);
    CHECK(cur.alloc(4) != nullptr);   // vuelve a caber entero
}

int main()
{
    test_misma_entrada_un_solo_grupo();
    test_mezcla_de_entradas_agrupa_contiguo();
    test_no_visibles_excluidos();
    test_base_de_first_instance();
    test_trunca_sin_desbordar();

    test_cursor_reparte_contiguo();
    test_cursor_no_cabe_devuelve_nulo();
    test_cursor_no_desborda_el_uint32();
    test_cursor_sin_buffer_no_reparte();
    test_cursor_rest_y_commit();
    test_cursor_reset_entre_frames();

    if (g_failures == 0) std::printf("instancing_tests: OK\n");
    else                 std::printf("instancing_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
