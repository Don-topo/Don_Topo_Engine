// Test headless del buffer de gizmos (sin GUI, sin device Vulkan). La parte de
// acumulacion —drawX, el flag de activado y el vaciado— no toca la API grafica,
// asi que se puede ejercitar aqui entera. Plain main + asserts, sin framework,
// coherente con frustum_tests.cpp.
//
// Lo que se prueba es la invariante que H16 convierte en imposible de romper:
// CONSUMIR VACIA. El bug ya paso una vez —el backend de DirectX 12 leia los
// vertices y no llamaba a clear(), asi que el vector crecia frame a frame hasta
// reventar los 65536 y lo unico que se veia era el aviso de capacidad—, y se
// habia arreglado a mano anadiendo un cuarto clear() que tambien se puede
// olvidar. Aqui se afirma que ya no hace falta acordarse.
#include "DonTopo/Renderer/Gizmos.h"

#include <cstdio>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// El singleton se comparte entre casos: cada uno empieza vaciando, que ademas
// es la primera comprobacion de que discard() hace lo suyo.
static void empezarLimpio()
{
    Gizmos::setEnabled(true);
    Gizmos::discard();
}

static void test_acumula_y_take_vacia()
{
    empezarLimpio();

    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(1.0f));
    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f));

    // Dos lineas = cuatro vertices: el buffer es una lista de segmentos.
    std::vector<GizmoVertex> tomados = Gizmos::takeVertices();
    CHECK(tomados.size() == 4);
    CHECK(tomados[1].pos.x == 1.0f);
    CHECK(tomados[3].pos.y == 1.0f);

    // Y esto es lo que antes habia que recordar a mano: tras consumir, vacio.
    std::vector<GizmoVertex> segunda = Gizmos::takeVertices();
    CHECK(segunda.empty());
}

// draw() es el consumidor del camino de Vulkan, y tiene que vaciar IGUAL que
// takeVertices. Con el pipeline sin crear se sale antes de tocar la API —que es
// lo que permite llamarla aqui sin device—, y ese early-return es justo el que
// dejaba los vertices dentro: si el buffer no se vaciara ahi, un motor al que no
// se le ha llamado a init() acumularia para siempre.
static void test_draw_sin_pipeline_tambien_vacia()
{
    empezarLimpio();

    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1.0f));
    CHECK(Gizmos::takeVertices().size() == 2);   // estaban ahi

    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1.0f));
    Gizmos::draw(VK_NULL_HANDLE, glm::mat4(1.0f), 0);
    CHECK(Gizmos::takeVertices().empty());
}

static void test_discard_vacia_sin_consumir()
{
    empezarLimpio();

    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f));
    Gizmos::discard();
    CHECK(Gizmos::takeVertices().empty());
}

// Apagado no acumula: es lo que hace que el checkbox del editor no cueste
// memoria en vez de solo no dibujar.
static void test_apagado_no_acumula()
{
    empezarLimpio();

    Gizmos::setEnabled(false);
    CHECK(!Gizmos::isEnabled());
    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f));
    Gizmos::drawWireSphere(glm::mat4(1.0f), glm::vec3(0.0f), 1.0f, glm::vec3(1.0f));
    CHECK(Gizmos::takeVertices().empty());

    Gizmos::setEnabled(true);
    CHECK(Gizmos::isEnabled());
    Gizmos::drawLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f));
    CHECK(Gizmos::takeVertices().size() == 2);
}

// El tope existe para que un bucle desbocado no se coma la memoria. Se
// comprueba que corta, y sobre todo que NO crece por encima: ese era el sintoma
// con el que se manifestaba la fuga.
static void test_tope_de_capacidad()
{
    empezarLimpio();

    for (int i = 0; i < 40000; i++)
        Gizmos::drawLine(glm::vec3((float)i), glm::vec3((float)i + 1.0f), glm::vec3(1.0f));

    const size_t total = Gizmos::takeVertices().size();
    CHECK(total <= 65536);
    // Y que de verdad ha llegado al tope, no que se haya quedado corto por otra
    // razon: 40000 lineas son 80000 vertices, mas del doble del limite.
    CHECK(total > 60000);
}

int main()
{
    test_acumula_y_take_vacia();
    test_draw_sin_pipeline_tambien_vacia();
    test_discard_vacia_sin_consumir();
    test_apagado_no_acumula();
    test_tope_de_capacidad();

    if (g_failures == 0) std::printf("gizmos_tests: OK\n");
    else                 std::printf("gizmos_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
